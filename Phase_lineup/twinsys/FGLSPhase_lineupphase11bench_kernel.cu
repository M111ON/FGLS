/*
 * bench_kernel.cu — Phase 11 GPU Benchmark
 * ═════════════════════════════════════════
 * Measures:
 *   A) Baseline kernel (no WHE)
 *   B) WHE-enabled kernel
 *   C) XOR checksum only (device function microbench)
 *
 * Build (GTX 1050 Ti = sm_61):
 *   nvcc -O2 -arch=sm_61 -o bench_kernel bench_kernel.cu
 *   nvcc -O2 -arch=sm_61 -DENABLE_WHE -o bench_kernel_whe bench_kernel.cu
 *
 * Output: throughput MB/s + latency us per 4608B payload
 */

/* bypass VS2022 + CUDA 11.8 STL version check */
#define _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <cuda_runtime.h>

/* ── pull in kernel (self-contained) ─────────────────────────── */
#include "geo_kernel_whe.cu"

/* ── bench config ─────────────────────────────────────────────── */
#define BENCH_ITERS      10000u   /* warmup + timed */
#define BENCH_WARMUP     200u
#define BENCH_MODE       K_MODE_TRANSFORM

/* ── CUDA error check ─────────────────────────────────────────── */
#define CK(call) do { \
    cudaError_t _e = (call); \
    if (_e != cudaSuccess) { \
        fprintf(stderr, "CUDA error %s:%d — %s\n", \
                __FILE__, __LINE__, cudaGetErrorString(_e)); \
        return 1; \
    } \
} while(0)

/* ── XOR checksum microbench kernel ──────────────────────────── */
__global__
void xor_checksum_kernel(const uint8_t * __restrict__ in,
                               uint32_t * __restrict__ out,
                               uint32_t n_slots)
{
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= n_slots) return;
    const uint8_t *slot = in + idx * K_SLOT_BYTES;
    uint32_t acc = 0;
    for (uint32_t i = 0; i < K_SLOT_BYTES; i++)
        acc ^= ((uint32_t)slot[i] << ((i & 3u) * 8u));
    out[idx] = acc;
}

/* ── fill payload with deterministic data ────────────────────── */
static void fill_payload(uint8_t *p, uint32_t sz)
{
    for (uint32_t i = 0; i < sz; i++)
        p[i] = (uint8_t)(i * 0x9E ^ (i >> 8));

    /* fix checksum fields so verify passes */
    for (uint32_t c = 0; c < K_COSET_COUNT; c++) {
        for (uint32_t f = 0; f < K_FRUSTUM_MOUNT; f++) {
            uint8_t *slot = p + c * K_COSET_BYTES + f * K_SLOT_BYTES;
            uint64_t c0, c1, c2, c3;
            memcpy(&c0, slot +  0, 8);
            memcpy(&c1, slot +  8, 8);
            memcpy(&c2, slot + 16, 8);
            memcpy(&c3, slot + 24, 8);
            uint32_t crc = (uint32_t)(c0 ^ (c0>>32))
                         ^ (uint32_t)(c1 ^ (c1>>32))
                         ^ (uint32_t)(c2 ^ (c2>>32))
                         ^ (uint32_t)(c3 ^ (c3>>32));
            memcpy(slot + FSLOT_CRC_OFF, &crc, 4);
        }
    }
}

/* ── read cuda timer ─────────────────────────────────────────── */
static float run_bench(uint8_t *d_in, uint8_t *d_out,
                        uint8_t mode, uint32_t iters)
{
    cudaEvent_t t0, t1;
    cudaEventCreate(&t0);
    cudaEventCreate(&t1);

    dim3 grid(K_COSET_COUNT);
    dim3 block(K_THREADS);

    /* warmup */
    for (uint32_t i = 0; i < BENCH_WARMUP; i++)
        geo_cube_dispatch<<<grid, block>>>(d_in, d_out, mode);
    cudaDeviceSynchronize();

    cudaEventRecord(t0);
    for (uint32_t i = 0; i < iters; i++)
        geo_cube_dispatch<<<grid, block>>>(d_in, d_out, mode);
    cudaEventRecord(t1);
    cudaEventSynchronize(t1);

    float ms = 0;
    cudaEventElapsedTime(&ms, t0, t1);
    cudaEventDestroy(t0);
    cudaEventDestroy(t1);
    return ms;
}

static float run_xor_bench(uint8_t *d_in, uint32_t *d_crc, uint32_t iters)
{
    cudaEvent_t t0, t1;
    cudaEventCreate(&t0);
    cudaEventCreate(&t1);

    uint32_t n_slots = K_COSET_COUNT * K_FRUSTUM_MOUNT; /* 72 */
    dim3 grid((n_slots + 31) / 32);
    dim3 block(32);

    for (uint32_t i = 0; i < BENCH_WARMUP; i++)
        xor_checksum_kernel<<<grid, block>>>(d_in, d_crc, n_slots);
    cudaDeviceSynchronize();

    cudaEventRecord(t0);
    for (uint32_t i = 0; i < iters; i++)
        xor_checksum_kernel<<<grid, block>>>(d_in, d_crc, n_slots);
    cudaEventRecord(t1);
    cudaEventSynchronize(t1);

    float ms = 0;
    cudaEventElapsedTime(&ms, t0, t1);
    cudaEventDestroy(t0);
    cudaEventDestroy(t1);
    return ms;
}

int main(void)
{
    /* ── device info ── */
    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, 0);
    printf("═══════════════════════════════════════════════\n");
    printf("GPU  : %s\n", prop.name);
    printf("SM   : %d × SM%d.%d\n", prop.multiProcessorCount,
           prop.major, prop.minor);
    printf("VRAM : %zu MB\n", prop.totalGlobalMem >> 20);
    printf("═══════════════════════════════════════════════\n\n");

    /* ── alloc ── */
    uint8_t  h_in[K_PAYLOAD_BYTES];
    uint8_t  h_out[K_PAYLOAD_BYTES];
    fill_payload(h_in, K_PAYLOAD_BYTES);

    uint8_t  *d_in, *d_out;
    uint32_t *d_crc;
    CK(cudaMalloc(&d_in,  K_PAYLOAD_BYTES));
    CK(cudaMalloc(&d_out, K_PAYLOAD_BYTES));
    CK(cudaMalloc(&d_crc, K_COSET_COUNT * K_FRUSTUM_MOUNT * sizeof(uint32_t)));
    CK(cudaMemcpy(d_in, h_in, K_PAYLOAD_BYTES, cudaMemcpyHostToDevice));

    float ms;
    float bytes = (float)(K_PAYLOAD_BYTES * BENCH_ITERS);
    float us_per = 0;

#ifdef ENABLE_WHE
    printf("[WHE ENABLED]\n\n");
#else
    printf("[BASELINE — no WHE]\n\n");
#endif

    /* ── A: main kernel ── */
    ms = run_bench(d_in, d_out, BENCH_MODE, BENCH_ITERS);
    us_per = (ms * 1000.0f) / BENCH_ITERS;
    printf("A) geo_cube_dispatch  (TRANSFORM)\n");
    printf("   iters      : %u\n", BENCH_ITERS);
    printf("   total ms   : %.3f\n", ms);
    printf("   us/dispatch : %.3f us\n", us_per);
    printf("   throughput  : %.1f MB/s\n\n",
           bytes / (ms / 1000.0f) / (1024.0f * 1024.0f));

    /* ── B: verify mode ── */
    ms = run_bench(d_in, d_out, K_MODE_VERIFY, BENCH_ITERS);
    us_per = (ms * 1000.0f) / BENCH_ITERS;
    printf("B) geo_cube_dispatch  (VERIFY only)\n");
    printf("   us/dispatch : %.3f us\n", us_per);
    printf("   throughput  : %.1f MB/s\n\n",
           bytes / (ms / 1000.0f) / (1024.0f * 1024.0f));

    /* ── C: XOR checksum only ── */
    ms = run_xor_bench(d_in, d_crc, BENCH_ITERS);
    us_per = (ms * 1000.0f) / BENCH_ITERS;
    printf("C) xor_checksum_kernel (72 slots)\n");
    printf("   us/dispatch : %.3f us\n", us_per);
    printf("   throughput  : %.1f MB/s\n\n",
           bytes / (ms / 1000.0f) / (1024.0f * 1024.0f));

    /* ── verify correctness: check PAD field ── */
    CK(cudaMemcpy(h_out, d_out, K_PAYLOAD_BYTES, cudaMemcpyDeviceToHost));
    int bad = 0;
    for (uint32_t c = 0; c < K_COSET_COUNT; c++) {
        for (uint32_t f = 0; f < K_FRUSTUM_MOUNT; f++) {
            uint8_t *slot = h_out + c * K_COSET_BYTES + f * K_SLOT_BYTES;
            uint32_t pad;
            memcpy(&pad, slot + FSLOT_PAD_OFF, 4);
#ifdef ENABLE_WHE
            uint8_t verify_ok  = pad & 0xFFu;
            uint8_t suspicious = (pad >> 8) & 0xFFu;
            uint16_t violations = (uint16_t)(pad >> 16);
            if (!verify_ok) bad++;
            if (violations > 0)
                printf("  WHE deviation coset=%u face=%u violations=%u susp=%u\n",
                       c, f, violations, suspicious);
#else
            if (!pad) bad++;
#endif
        }
    }
    printf("Correctness: %s (%d bad slots)\n",
           bad == 0 ? "PASS ✓" : "FAIL ✗", bad);

    cudaFree(d_in); cudaFree(d_out); cudaFree(d_crc);
    return bad ? 1 : 0;
}
