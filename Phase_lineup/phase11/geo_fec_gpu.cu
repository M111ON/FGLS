/*
 * geo_fec_gpu.cu — GPU XOR FEC encode (Phase 11 port)
 * ═════════════════════════════════════════════════════
 * Ports fec_encode_all() hot path to CUDA.
 *
 * Layout:
 *   FEC_LEVELS=5, FEC_BLOCKS_PER_LEVEL=12 → 60 blocks total
 *   FEC_CHUNKS_PER_BLOCK=12, TSTREAM_DATA_BYTES=4096
 *   FEC_PARITY_PER_BLOCK=3 (P0=all, P1=stride3A, P2=stride3B)
 *
 * Kernel launch: <<<5, 12>>> → 1 thread per block (level=blockIdx, block=threadIdx)
 *
 * Input:  store_flat — 720 × 4096B = 2,949,120B
 *         sizes_flat — 720 × uint16_t
 * Output: parity_flat — 180 × (3×4096 + meta) FECParityGPU
 *
 * Build:
 *   nvcc -O2 -arch=sm_61 -D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH
 *        -allow-unsupported-compiler -o bench_fec_gpu bench_fec_gpu.cu
 */

#define _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <cuda_runtime.h>

/* ── constants (must match geo_fec.h) ────────────────────────── */
#define FEC_CHUNKS_PER_BLOCK   12u
#define FEC_BLOCKS_PER_LEVEL   12u
#define FEC_LEVELS              5u
#define FEC_PARITY_PER_BLOCK    3u
#define FEC_TOTAL_DATA        720u
#define FEC_TOTAL_BLOCKS       60u   /* 5 × 12 */
#define FEC_TOTAL_PARITY      180u   /* 60 × 3 */
#define TSTREAM_DATA_BYTES   4096u
#define GEO_PYR_PHASE_LEN     144u   /* chunks per level */

/* stride masks */
#define S3A_MASK  0x249u   /* bits 0,3,6,9   → chunks 0,3,6,9   */
#define S3B_MASK  0x492u   /* bits 1,4,7,10  → chunks 1,4,7,10  */

/* ── GPU parity struct — explicit layout, 32B aligned ───────── */
#define FECPAR_DATA_OFF   0u
#define FECPAR_SIZES_OFF  (3u * TSTREAM_DATA_BYTES)          /* 12288 */
#define FECPAR_META_OFF   (FECPAR_SIZES_OFF + FEC_CHUNKS_PER_BLOCK * 2u) /* 12312 */
#define FECPAR_STRIDE     12320u   /* pad to 32B boundary */

/* host-side struct for sizeof/memcpy — must match FECPAR_STRIDE */
typedef struct {
    uint8_t  data[FEC_PARITY_PER_BLOCK][TSTREAM_DATA_BYTES]; /* @0    12288B */
    uint16_t chunk_sizes[FEC_CHUNKS_PER_BLOCK];               /* @12288  24B */
    uint8_t  level;                                            /* @12312   1B */
    uint8_t  block;                                            /* @12313   1B */
    uint8_t  _pad[6];                                          /* @12314   6B → total 12320 */
} FECParityGPU;

/* ── static assert struct size ───────────────────────────────── */
#ifndef __CUDACC__
typedef char _fecpar_sz[(sizeof(FECParityGPU) == FECPAR_STRIDE) ? 1 : -1];
#endif
#define CK(call) do { \
    cudaError_t _e = (call); \
    if (_e != cudaSuccess) { \
        fprintf(stderr, "CUDA %s:%d — %s\n", __FILE__, __LINE__, \
                cudaGetErrorString(_e)); return 1; } \
} while(0)

/* ═══════════════════════════════════════════════════════════════
 * KERNEL: fec_xor_encode_all  (v2 — parallel per byte position)
 *
 *   gridDim  = (FEC_LEVELS=5, FEC_BLOCKS_PER_LEVEL=12)
 *   blockDim = (128)  → 128 threads cover 4096B in 32B steps
 *
 *   thread tid covers bytes [tid*32 .. tid*32+31] of P0/P1/P2
 *   → 60 blocks × 128 threads = 7680 threads total
 *   → fully parallel across blocks AND within block
 * ═══════════════════════════════════════════════════════════════ */
#define THREADS_PER_BLOCK  128u
#define BYTES_PER_THREAD   (TSTREAM_DATA_BYTES / THREADS_PER_BLOCK)  /* 32 */

__global__
void fec_xor_encode_all(
    const uint8_t  * __restrict__ store,   /* [720][4096] flat */
    const uint16_t * __restrict__ sizes,   /* [720] */
          uint8_t  * __restrict__ parity)  /* [60][FECParityGPU] */
{
    uint32_t level = blockIdx.x;    /* 0..4  */
    uint32_t blk   = blockIdx.y;    /* 0..11 */
    uint32_t tid   = threadIdx.x;   /* 0..127 */

    uint32_t base    = level * GEO_PYR_PHASE_LEN + blk * FEC_CHUNKS_PER_BLOCK;
    uint32_t par_idx = level * FEC_BLOCKS_PER_LEVEL + blk;
    uint8_t *par_out = parity + par_idx * FECPAR_STRIDE;

    uint8_t  *p0  = par_out + 0 * TSTREAM_DATA_BYTES;
    uint8_t  *p1  = par_out + 1 * TSTREAM_DATA_BYTES;
    uint8_t  *p2  = par_out + 2 * TSTREAM_DATA_BYTES;

    /* byte range this thread owns */
    uint32_t bstart = tid * BYTES_PER_THREAD;   /* 0,32,64,...,4064 */

    /* accumulators — work in uint64 for 8× throughput */
    uint64_t acc0[4], acc1[4], acc2[4];
    acc0[0]=acc0[1]=acc0[2]=acc0[3]=0;
    acc1[0]=acc1[1]=acc1[2]=acc1[3]=0;
    acc2[0]=acc2[1]=acc2[2]=acc2[3]=0;

    /* XOR 12 chunks at this byte range */
    for (uint8_t i = 0; i < FEC_CHUNKS_PER_BLOCK; i++) {
        uint32_t pos = base + i;
        const uint64_t *src = (const uint64_t *)(store
                               + pos * TSTREAM_DATA_BYTES + bstart);
        uint32_t mask = 1u << i;

        /* P0: all chunks */
        acc0[0] ^= src[0]; acc0[1] ^= src[1];
        acc0[2] ^= src[2]; acc0[3] ^= src[3];

        /* P1: S3A mask (chunks 0,3,6,9) */
        if (mask & S3A_MASK) {
            acc1[0] ^= src[0]; acc1[1] ^= src[1];
            acc1[2] ^= src[2]; acc1[3] ^= src[3];
        }
        /* P2: S3B mask (chunks 1,4,7,10) */
        if (mask & S3B_MASK) {
            acc2[0] ^= src[0]; acc2[1] ^= src[1];
            acc2[2] ^= src[2]; acc2[3] ^= src[3];
        }
    }

    /* write 32B back */
    uint64_t *dst0 = (uint64_t *)(p0 + bstart);
    uint64_t *dst1 = (uint64_t *)(p1 + bstart);
    uint64_t *dst2 = (uint64_t *)(p2 + bstart);
    dst0[0]=acc0[0]; dst0[1]=acc0[1]; dst0[2]=acc0[2]; dst0[3]=acc0[3];
    dst1[0]=acc1[0]; dst1[1]=acc1[1]; dst1[2]=acc1[2]; dst1[3]=acc1[3];
    dst2[0]=acc2[0]; dst2[1]=acc2[1]; dst2[2]=acc2[2]; dst2[3]=acc2[3];

    if (tid == 0) {
        uint16_t *csz = (uint16_t *)(par_out + FECPAR_SIZES_OFF);
        for (uint8_t i = 0; i < FEC_CHUNKS_PER_BLOCK; i++)
            csz[i] = sizes[base + i];
        par_out[FECPAR_META_OFF]     = (uint8_t)level;
        par_out[FECPAR_META_OFF + 1] = (uint8_t)blk;
    }
}

/* ═══════════════════════════════════════════════════════════════
 * HOST bench
 * ═══════════════════════════════════════════════════════════════ */
int main(void)
{
    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, 0);
    printf("═══════════════════════════════════════════════\n");
    printf("GPU  : %s  SM%d.%d\n", prop.name, prop.major, prop.minor);
    printf("═══════════════════════════════════════════════\n\n");

    /* ── alloc host ── */
    size_t store_sz  = (size_t)FEC_TOTAL_DATA * TSTREAM_DATA_BYTES;  /* ~2.8MB */
    size_t sizes_sz  = (size_t)FEC_TOTAL_DATA * sizeof(uint16_t);
    size_t parity_sz = (size_t)FEC_TOTAL_PARITY * FECPAR_STRIDE;

    uint8_t  *h_store  = (uint8_t  *)malloc(store_sz);
    uint16_t *h_sizes  = (uint16_t *)malloc(sizes_sz);
    uint8_t  *h_parity = (uint8_t  *)malloc(parity_sz);

    /* fill with deterministic data */
    for (size_t i = 0; i < store_sz; i++)
        h_store[i] = (uint8_t)(i ^ (i >> 8));
    for (uint32_t i = 0; i < FEC_TOTAL_DATA; i++)
        h_sizes[i] = TSTREAM_DATA_BYTES;

    /* ── alloc device ── */
    uint8_t  *d_store, *d_parity;
    uint16_t *d_sizes;
    CK(cudaMalloc(&d_store,  store_sz));
    CK(cudaMalloc(&d_sizes,  sizes_sz));
    CK(cudaMalloc(&d_parity, parity_sz));
    CK(cudaMemcpy(d_store, h_store, store_sz, cudaMemcpyHostToDevice));
    CK(cudaMemcpy(d_sizes, h_sizes, sizes_sz, cudaMemcpyHostToDevice));

    /* ── bench ── */
    #define ITERS   5000u
    #define WARMUP   100u

    dim3 grid(FEC_LEVELS, FEC_BLOCKS_PER_LEVEL);
    dim3 block(THREADS_PER_BLOCK);

    /* warmup */
    for (uint32_t i = 0; i < WARMUP; i++)
        fec_xor_encode_all<<<grid, block>>>(d_store, d_sizes, d_parity);
    cudaDeviceSynchronize();

    cudaEvent_t t0, t1;
    cudaEventCreate(&t0); cudaEventCreate(&t1);
    cudaEventRecord(t0);
    for (uint32_t i = 0; i < ITERS; i++)
        fec_xor_encode_all<<<grid, block>>>(d_store, d_sizes, d_parity);
    cudaEventRecord(t1);
    cudaEventSynchronize(t1);

    float ms = 0;
    cudaEventElapsedTime(&ms, t0, t1);
    cudaEventDestroy(t0); cudaEventDestroy(t1);

    float us_per  = (ms * 1000.0f) / ITERS;
    float bytes   = (float)(store_sz * ITERS);
    float tput    = bytes / (ms / 1000.0f) / (1024.0f * 1024.0f);

    printf("fec_xor_encode_all  (60 blocks × 12 chunks × 4096B)\n");
    printf("  iters       : %u\n", ITERS);
    printf("  total ms    : %.3f\n", ms);
    printf("  us/encode   : %.3f us\n", us_per);
    printf("  throughput  : %.1f MB/s\n\n", tput);

    /* ── correctness: CPU vs GPU P0 spot check ── */
    CK(cudaMemcpy(h_parity, d_parity, parity_sz, cudaMemcpyDeviceToHost));

    int bad = 0;
    for (uint32_t l = 0; l < FEC_LEVELS && bad == 0; l++) {
        for (uint32_t b = 0; b < FEC_BLOCKS_PER_LEVEL && bad == 0; b++) {
            uint32_t base = l * GEO_PYR_PHASE_LEN + b * FEC_CHUNKS_PER_BLOCK;
            /* compute P0 on CPU */
            uint8_t cpu_p0[TSTREAM_DATA_BYTES] = {0};
            for (uint8_t i = 0; i < FEC_CHUNKS_PER_BLOCK; i++) {
                uint32_t pos = base + i;
                for (uint32_t x = 0; x < TSTREAM_DATA_BYTES; x++)
                    cpu_p0[x] ^= h_store[pos * TSTREAM_DATA_BYTES + x];
            }
            /* compare with GPU P0 */
            uint32_t par_idx = l * FEC_BLOCKS_PER_LEVEL + b;
            uint8_t *gpu_p0  = h_parity + par_idx * FECPAR_STRIDE;
            if (memcmp(cpu_p0, gpu_p0, TSTREAM_DATA_BYTES) != 0) {
                printf("MISMATCH level=%u block=%u\n", l, b);
                bad++;
            }
        }
    }
    printf("Correctness P0: %s\n", bad == 0 ? "PASS ✓" : "FAIL ✗");

    free(h_store); free(h_sizes); free(h_parity);
    cudaFree(d_store); cudaFree(d_sizes); cudaFree(d_parity);
    return bad;
}
