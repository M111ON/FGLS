/*
 * geo_kernel_multigpu.cu — FGLS Dual GPU Pipeline
 * ═════════════════════════════════════════════════
 * Split batch across GPU 0 + GPU 1 equally
 * Each GPU runs same seed-only kernel independently
 * Host syncs both then merges results
 *
 * No malloc. No float. No heap (device side).
 * ═════════════════════════════════════════════════
 */

#include <stdint.h>
#include <cuda_runtime.h>

#ifdef _WIN32
#  define FGLS_EXPORT __declspec(dllexport)
#else
#  define FGLS_EXPORT
#endif

/* ── Constants ───────────────────────────────────────────────── */
#define K_COSET_COUNT    12u
#define K_FRUSTUM_MOUNT   6u
#define K_THREADS        32u
#define K_PHI_SCALE      (1u << 20)
#define K_PHI_UP         1696631u
#define K_PHI_DOWN        648055u

/* ── Structs ─────────────────────────────────────────────────── */
#pragma pack(push, 1)
typedef struct {
    uint64_t seed;
    uint32_t dispatch_id;
} ApexSeed;   /* 12B */
#pragma pack(pop)

typedef struct {
    uint32_t coset_checksum[K_COSET_COUNT];
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

__device__ __forceinline__
uint32_t _d_coset_checksum(uint64_t seed, uint8_t coset)
{
    uint64_t cseed     = _d_derive(seed, coset, 0u);
    uint64_t acc       = cseed;
    uint32_t coset_chk = (uint32_t)(cseed ^ (cseed >> 32));
    for (uint8_t f = 0; f < K_FRUSTUM_MOUNT; f++) {
        uint64_t cur = _d_derive(acc, f, (uint32_t)coset + 1u);
        for (uint8_t lv = 0; lv < 4u; lv++) {
            coset_chk ^= (uint32_t)(cur ^ (cur >> 32));
            cur        = _d_derive(cur, f, (uint32_t)(lv + 1u));
        }
        acc = cur;
    }
    return coset_chk;
}

/* ── Kernel ──────────────────────────────────────────────────── */
__global__
void geo_seed_dispatch(const ApexSeed * __restrict__ seeds,
                             ApexResult * __restrict__ results)
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
        r->master_fold  = master;
        r->verify_ok    = (master != 0u) ? 1u : 0u;
        r->dispatch_id  = dispatch_id;
        r->_pad         = 0u;
    }
}

/* ── Multi-GPU pipeline ──────────────────────────────────────── */
#ifdef __cplusplus
extern "C"
#endif
FGLS_EXPORT int geo_multigpu_pipeline(const ApexSeed *h_seeds,
                                            ApexResult *h_results,
                                            uint32_t    batch_size,
                                            int         n_gpus)
{
    if (n_gpus < 1 || n_gpus > 2) n_gpus = 1;

    /* split batch */
    uint32_t half0 = batch_size / (uint32_t)n_gpus;
    uint32_t half1 = batch_size - half0;

    size_t in0  = (size_t)half0 * sizeof(ApexSeed);
    size_t out0 = (size_t)half0 * sizeof(ApexResult);
    size_t in1  = (size_t)half1 * sizeof(ApexSeed);
    size_t out1 = (size_t)half1 * sizeof(ApexResult);

    ApexSeed   *d_seeds0 = NULL, *d_seeds1 = NULL;
    ApexResult *d_res0   = NULL, *d_res1   = NULL;

    /* GPU 0 */
    cudaSetDevice(0);
    cudaMalloc(&d_seeds0, in0);
    cudaMalloc(&d_res0,   out0);
    cudaMemcpy(d_seeds0, h_seeds, in0, cudaMemcpyHostToDevice);
    geo_seed_dispatch<<<half0, K_THREADS>>>(d_seeds0, d_res0);

    /* GPU 1 (if available) */
    if (n_gpus == 2) {
        cudaSetDevice(1);
        cudaMalloc(&d_seeds1, in1);
        cudaMalloc(&d_res1,   out1);
        cudaMemcpy(d_seeds1, h_seeds + half0, in1, cudaMemcpyHostToDevice);
        geo_seed_dispatch<<<half1, K_THREADS>>>(d_seeds1, d_res1);
    }

    /* sync + D2H GPU 0 */
    cudaSetDevice(0);
    cudaDeviceSynchronize();
    cudaMemcpy(h_results, d_res0, out0, cudaMemcpyDeviceToHost);
    cudaFree(d_seeds0);
    cudaFree(d_res0);

    /* sync + D2H GPU 1 */
    if (n_gpus == 2) {
        cudaSetDevice(1);
        cudaDeviceSynchronize();
        cudaMemcpy(h_results + half0, d_res1, out1, cudaMemcpyDeviceToHost);
        cudaFree(d_seeds1);
        cudaFree(d_res1);
    }

    cudaError_t err = cudaGetLastError();
    return (err == cudaSuccess) ? 0 : (int)err;
}

/* ── Single GPU fallback (same as v2) ────────────────────────── */
#ifdef __cplusplus
extern "C"
#endif
FGLS_EXPORT int geo_seed_pipeline(const ApexSeed *h_seeds,
                                        ApexResult *h_results,
                                        uint32_t    batch_size)
{
    return geo_multigpu_pipeline(h_seeds, h_results, batch_size, 1);
}
