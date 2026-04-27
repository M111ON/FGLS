/*
 * bench_whe_kernel.cu — Phase 11 GPU Benchmark
 * ═════════════════════════════════════════════
 * Measures:
 *   1. Baseline kernel (no WHE)  — throughput MB/s + latency us
 *   2. WHE-enabled kernel        — throughput MB/s + latency us
 *   3. Delta cost of WHE
 *
 * Build (no WHE):  nvcc -O2 -o bench_baseline bench_whe_kernel.cu
 * Build (WHE):     nvcc -O2 -DENABLE_WHE -o bench_whe bench_whe_kernel.cu
 *
 * Run: ./bench_baseline   or   ./bench_whe
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <cuda_runtime.h>

/* ── embed kernel (single-file bench) ── */
#include "geo_kernel_whe.cu"

/* ── bench config ── */
#define BENCH_ITERS     10000u   /* kernel launches */
#define BENCH_WARMUP    200u     /* warmup launches (not measured) */
#define PAYLOAD_BYTES   K_PAYLOAD_BYTES  /* 4608B */

/* ── host helpers ── */
static void fill_payload(uint8_t *buf) {
    /* deterministic fill: core values + valid checksums */
    for (uint32_t c = 0; c < K_COSET_COUNT; c++) {
        for (uint32_t f = 0; f < K_FRUSTUM_MOUNT; f++) {
            uint8_t *slot = buf + c * K_COSET_BYTES + f * K_SLOT_BYTES;
            memset(slot, 0, K_SLOT_BYTES);
            /* core[0..3] */
            uint64_t core[4];
            for (int i = 0; i < 4; i++)
                core[i] = (uint64_t)(c * 6 + f + 1) * 0x9E3779B97F4A7C15ULL * (i+1);
            memcpy(slot + FSLOT_CORE0_OFF, core, 32);
            /* face_id */
            slot[FSLOT_FRUST_OFF] = (uint8_t)f;
            /* valid checksum */
            uint32_t crc = (uint32_t)(core[0] ^ (core[0]>>32))
                         ^ (uint32_t)(core[1] ^ (core[1]>>32))
                         ^ (uint32_t)(core[2] ^ (core[2]>>32))
                         ^ (uint32_t)(core[3] ^ (core[3]>>32));
            memcpy(slot + FSLOT_CRC_OFF, &crc, 4);
        }
    }
}

static void check_cuda(cudaError_t e, const char *msg) {
    if (e != cudaSuccess) {
        fprintf(stderr, "CUDA error %s: %s\n", msg, cudaGetErrorString(e));
        exit(1);
    }
}

int main(void) {
#ifdef ENABLE_WHE
    const char *mode_str = "WHE-ENABLED";
#else
    const char *mode_str = "BASELINE (no WHE)";
#endif

    printf("═══════════════════════════════════════\n");
    printf(" geo_cube_dispatch bench — %s\n", mode_str);
    printf(" payload=%.0fB  iters=%u  warmup=%u\n",
           (double)PAYLOAD_BYTES, BENCH_ITERS, BENCH_WARMUP);
    printf("═══════════════════════════════════════\n");

    /* alloc host */
    uint8_t *h_in  = (uint8_t*)malloc(PAYLOAD_BYTES);
    uint8_t *h_out = (uint8_t*)malloc(PAYLOAD_BYTES);
    fill_payload(h_in);

    /* alloc device */
    uint8_t *d_in, *d_out;
    check_cuda(cudaMalloc(&d_in,  PAYLOAD_BYTES), "malloc d_in");
    check_cuda(cudaMalloc(&d_out, PAYLOAD_BYTES), "malloc d_out");
    check_cuda(cudaMemcpy(d_in, h_in, PAYLOAD_BYTES, cudaMemcpyHostToDevice), "H2D");

    dim3 grid(K_COSET_COUNT);   /* 12 blocks */
    dim3 block(K_THREADS);      /* 32 threads */

    /* warmup */
    for (uint32_t i = 0; i < BENCH_WARMUP; i++)
        geo_cube_dispatch<<<grid, block>>>(d_in, d_out, K_MODE_TRANSFORM);
    cudaDeviceSynchronize();

    /* ── timed run ── */
    cudaEvent_t t0, t1;
    cudaEventCreate(&t0);
    cudaEventCreate(&t1);

    cudaEventRecord(t0);
    for (uint32_t i = 0; i < BENCH_ITERS; i++)
        geo_cube_dispatch<<<grid, block>>>(d_in, d_out, K_MODE_TRANSFORM);
    cudaEventRecord(t1);
    cudaDeviceSynchronize();

    float ms = 0.0f;
    cudaEventElapsedTime(&ms, t0, t1);

    double total_us   = ms * 1000.0;
    double per_launch = total_us / BENCH_ITERS;
    double bytes_per  = (double)PAYLOAD_BYTES * BENCH_ITERS;
    double throughput = bytes_per / (ms / 1000.0) / (1024.0*1024.0);

    printf(" Total    : %.2f ms\n", ms);
    printf(" Per call : %.3f us\n", per_launch);
    printf(" Throughput: %.1f MB/s\n", throughput);

    /* ── verify WHE flags on sample slot ── */
    check_cuda(cudaMemcpy(h_out, d_out, PAYLOAD_BYTES, cudaMemcpyDeviceToHost), "D2H");

    printf("\n── Sample PAD values (coset 0, face 0..5) ──\n");
    for (uint32_t f = 0; f < K_FRUSTUM_MOUNT; f++) {
        uint8_t *slot = h_out + f * K_SLOT_BYTES;
        uint32_t pad;
        memcpy(&pad, slot + FSLOT_PAD_OFF, 4);
#ifdef ENABLE_WHE
        printf("  face%u: violations=%u suspicious=%u verify=%u\n",
               f,
               geo_whe_pad_violations(pad),
               geo_whe_pad_suspicious(pad),
               geo_whe_pad_verify(pad));
#else
        printf("  face%u: verify=%u\n", f, pad & 1u);
#endif
    }

    /* ── also bench VERIFY mode ── */
    printf("\n── VERIFY mode ──\n");
    cudaEventRecord(t0);
    for (uint32_t i = 0; i < BENCH_ITERS; i++)
        geo_cube_dispatch<<<grid, block>>>(d_in, d_out, K_MODE_VERIFY);
    cudaEventRecord(t1);
    cudaDeviceSynchronize();
    cudaEventElapsedTime(&ms, t0, t1);
    printf(" Per call : %.3f us\n", ms * 1000.0 / BENCH_ITERS);
    printf(" Throughput: %.1f MB/s\n",
           (double)PAYLOAD_BYTES * BENCH_ITERS / (ms/1000.0) / (1024.0*1024.0));

    cudaFree(d_in); cudaFree(d_out);
    free(h_in); free(h_out);
    cudaEventDestroy(t0); cudaEventDestroy(t1);

    printf("\nDONE\n");
    return 0;
}
