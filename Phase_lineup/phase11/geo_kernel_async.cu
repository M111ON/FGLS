/*
 * geo_kernel_async.cu — FGLS Async Multi-Stream Pipeline
 * ═══════════════════════════════════════════════════════
 *
 * Goal: overlap H2D + compute + D2H across N streams
 *       hide PCIe latency behind GPU compute
 *
 * Pipeline per stream (3-stage):
 *   [H2D seeds] → [kernel] → [D2H results]
 *   Stream k+1 starts H2D while stream k runs kernel
 *   Stream k+2 starts H2D while stream k+1 runs kernel
 *                            while stream k does D2H
 *
 * Transfer per cube: 12B in + 64B out = 76B (seed-only)
 * Kernel: same geo_seed_dispatch logic (race-free v2)
 *
 * Config:
 *   N_STREAMS = 4  (sweet spot for T4)
 *   chunk     = batch_size / N_STREAMS
 *   pinned memory on host (mandatory for async DMA)
 *
 * No malloc. No float. No heap (device side).
 * ═══════════════════════════════════════════════════════
 */

#include <stdint.h>
#include <cuda_runtime.h>

/* ── Constants ───────────────────────────────────────────────── */
#define K_COSET_COUNT    12u
#define K_FRUSTUM_MOUNT   6u
#define K_THREADS        32u
#define K_PHI_SCALE      (1u << 20)
#define K_PHI_UP         1696631u
#define K_PHI_DOWN        648055u
#define K_N_STREAMS       4u

/* ── Structs ─────────────────────────────────────────────────── */
typedef struct {
    uint64_t seed;
    uint32_t dispatch_id;
} ApexSeed;   /* 12B */

typedef struct {
    uint32_t coset_checksum[K_COSET_COUNT];  /* 48B */
    uint32_t master_fold;
    uint32_t verify_ok;
    uint32_t dispatch_id;
    uint32_t _pad;
} ApexResult;  /* 64B */

/* ── Device primitives ───────────────────────────────────────── */
__device__ __forceinline__
uint64_t _d_mix64(uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31; return x;
}

__device__ __forceinline__
uint64_t _d_rotl64(uint64_t x, int r) {
    return (x << r) | (x >> (64 - r));
}

__device__ __forceinline__
uint64_t _d_derive(uint64_t core, uint8_t face, uint32_t step) {
    uint64_t salt = ((uint64_t)face << 56) ^ (uint64_t)step;
    uint64_t a    = _d_mix64(core ^ salt);
    uint64_t b    = _d_rotl64(core, (int)((face + step) & 63u));
    return _d_mix64(a ^ b);
}

/* noinline = avoid forceinline optimization bug (v2 fix) */
__device__ __noinline__
uint32_t _d_coset_checksum(uint64_t seed, uint8_t coset)
{
    uint64_t cseed     = _d_derive(seed, coset, 0u);
    uint32_t coset_chk = 0u;
    for (uint8_t f = 0; f < K_FRUSTUM_MOUNT; f++) {
        uint64_t cur      = _d_derive(cseed, f, 0u);
        uint32_t slot_chk = 0u;
        for (uint8_t lv = 0; lv < 4u; lv++) {
            slot_chk ^= (uint32_t)(cur ^ (cur >> 32));
            cur        = _d_derive(cur, f, lv + 1u);
        }
        coset_chk ^= slot_chk;
    }
    return coset_chk;
}

/* ── Kernel (same as v2, race-free) ─────────────────────────── */
__global__
void geo_seed_dispatch_async(const ApexSeed * __restrict__ seeds,
                                   ApexResult * __restrict__ results,
                                   uint32_t offset)
{
    __shared__ uint32_t s_chk[K_COSET_COUNT];

    uint32_t bid = blockIdx.x;
    uint32_t tid = threadIdx.x;

    uint64_t seed        = seeds[bid].seed;
    uint32_t dispatch_id = seeds[bid].dispatch_id;

    if (tid < K_COSET_COUNT)
        s_chk[tid] = _d_coset_checksum(seed, (uint8_t)tid);

    __syncthreads();

    if (tid == 0u) {
        ApexResult *r   = &results[bid];
        uint32_t master = 0u;
        for (uint8_t c = 0; c < K_COSET_COUNT; c++) {
            r->coset_checksum[c] = s_chk[c];
            master ^= s_chk[c];
        }
        uint32_t ref    = _d_coset_checksum(seed, 0u);
        r->master_fold  = master;
        r->verify_ok    = (ref == s_chk[0]) ? 1u : 0u;
        r->dispatch_id  = dispatch_id;
        r->_pad         = 0u;
    }
}

/* ── Async pipeline context ──────────────────────────────────── */
typedef struct {
    cudaStream_t streams[K_N_STREAMS];
    ApexSeed    *d_seeds[K_N_STREAMS];    /* device per stream    */
    ApexResult  *d_results[K_N_STREAMS];  /* device per stream    */
    uint32_t     chunk_size;              /* cubes per stream     */
    int          ready;
} AsyncCtx;

/* ── async_ctx_init: alloc pinned + device buffers ───────────── */
#ifdef __cplusplus
extern "C"
#endif
int async_ctx_init(AsyncCtx *ctx, uint32_t chunk_size)
{
    ctx->chunk_size = chunk_size;
    ctx->ready      = 0;

    for (uint32_t s = 0; s < K_N_STREAMS; s++) {
        if (cudaStreamCreate(&ctx->streams[s]) != cudaSuccess) return 1;

        size_t in_bytes  = (size_t)chunk_size * sizeof(ApexSeed);
        size_t out_bytes = (size_t)chunk_size * sizeof(ApexResult);

        if (cudaMalloc(&ctx->d_seeds[s],   in_bytes)  != cudaSuccess) return 2;
        if (cudaMalloc(&ctx->d_results[s], out_bytes) != cudaSuccess) return 3;
    }
    ctx->ready = 1;
    return 0;
}

/* ── async_ctx_free ──────────────────────────────────────────── */
#ifdef __cplusplus
extern "C"
#endif
void async_ctx_free(AsyncCtx *ctx)
{
    for (uint32_t s = 0; s < K_N_STREAMS; s++) {
        if (ctx->d_seeds[s])   cudaFree(ctx->d_seeds[s]);
        if (ctx->d_results[s]) cudaFree(ctx->d_results[s]);
        cudaStreamDestroy(ctx->streams[s]);
    }
    ctx->ready = 0;
}

/* ── geo_async_pipeline: main overlap pipeline ───────────────── */
/*
 * h_seeds, h_results MUST be pinned memory (cudaMallocHost)
 * batch_size must be divisible by K_N_STREAMS for clean chunks
 *
 * Stream rotation:
 *   s=0: H2D chunk0 → kernel chunk0 → D2H chunk0
 *   s=1: H2D chunk1 (overlap with kernel chunk0) → ...
 *   ...
 */
#ifdef __cplusplus
extern "C"
#endif
cudaError_t geo_async_pipeline(AsyncCtx         *ctx,
                                const ApexSeed   *h_seeds,
                                      ApexResult *h_results,
                                      uint32_t    batch_size)
{
    if (!ctx->ready) return cudaErrorNotReady;

    uint32_t chunk   = ctx->chunk_size;
    uint32_t n_chunk = (batch_size + chunk - 1) / chunk;

    for (uint32_t i = 0; i < n_chunk; i++) {
        uint32_t s      = i % K_N_STREAMS;
        uint32_t offset = i * chunk;
        uint32_t count  = (offset + chunk <= batch_size)
                        ? chunk : (batch_size - offset);

        size_t in_bytes  = (size_t)count * sizeof(ApexSeed);
        size_t out_bytes = (size_t)count * sizeof(ApexResult);

        /* H2D async */
        cudaMemcpyAsync(ctx->d_seeds[s],
                        h_seeds + offset,
                        in_bytes,
                        cudaMemcpyHostToDevice,
                        ctx->streams[s]);

        /* kernel async */
        dim3 grid(count, 1, 1);
        dim3 block(K_THREADS, 1, 1);
        geo_seed_dispatch_async<<<grid, block, 0, ctx->streams[s]>>>(
            ctx->d_seeds[s], ctx->d_results[s], offset
        );

        /* D2H async */
        cudaMemcpyAsync(h_results + offset,
                        ctx->d_results[s],
                        out_bytes,
                        cudaMemcpyDeviceToHost,
                        ctx->streams[s]);
    }

    /* sync all streams */
    for (uint32_t s = 0; s < K_N_STREAMS; s++)
        cudaStreamSynchronize(ctx->streams[s]);

    return cudaGetLastError();
}

/* ── One-shot helper (alloc pinned + run + free) ─────────────── */
#ifdef __cplusplus
extern "C"
#endif
int geo_async_oneshot(const ApexSeed *h_seeds_pageable,
                            ApexResult *h_results_pageable,
                            uint32_t    batch_size,
                            uint32_t    chunk_size)
{
    /* alloc pinned */
    ApexSeed   *h_seeds_pin   = NULL;
    ApexResult *h_results_pin = NULL;

    size_t in_bytes  = (size_t)batch_size * sizeof(ApexSeed);
    size_t out_bytes = (size_t)batch_size * sizeof(ApexResult);

    if (cudaMallocHost(&h_seeds_pin,   in_bytes)  != cudaSuccess) return 1;
    if (cudaMallocHost(&h_results_pin, out_bytes) != cudaSuccess) {
        cudaFreeHost(h_seeds_pin); return 2;
    }

    /* copy input to pinned */
    memcpy(h_seeds_pin, h_seeds_pageable, in_bytes);

    /* init ctx */
    AsyncCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    if (async_ctx_init(&ctx, chunk_size) != 0) {
        cudaFreeHost(h_seeds_pin);
        cudaFreeHost(h_results_pin);
        return 3;
    }

    /* run */
    cudaError_t err = geo_async_pipeline(&ctx,
                                          h_seeds_pin,
                                          h_results_pin,
                                          batch_size);

    /* copy result back */
    memcpy(h_results_pageable, h_results_pin, out_bytes);

    async_ctx_free(&ctx);
    cudaFreeHost(h_seeds_pin);
    cudaFreeHost(h_results_pin);

    return (err == cudaSuccess) ? 0 : (int)err;
}
