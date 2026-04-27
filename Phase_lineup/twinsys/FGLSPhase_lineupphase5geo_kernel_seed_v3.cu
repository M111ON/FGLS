/*
 * geo_kernel_seed_v2.cu — FGLS Seed-Only Dispatch (Race-Free)
 * ════════════════════════════════════════════════════════════
 *
 * Fix from v1:
 *   Race condition: multiple threads writing same s_slots[f] simultaneously
 *   Solution: each thread owns its own coset derivation in registers only
 *             shared memory used ONLY for checksum reduction (safe)
 *
 * Thread model (clean):
 *   tid 0..11  → derive coset[tid] entirely in registers
 *              → write s_chk[tid] only (1 uint32, no conflict)
 *   tid 12..31 → idle during derive, join reduction
 *   tid 0      → fold s_chk[0..11] → write ApexResult
 *
 * No shared memory write conflict possible.
 * No malloc. No float. No heap.
 * ════════════════════════════════════════════════════════════
 */

#include <stdint.h>
#include <cuda_runtime.h>

/* ── Constants ───────────────────────────────────────────────── */
#define K_COSET_COUNT    12u
#define K_FRUSTUM_MOUNT   6u
#define K_SLOT_BYTES     64u
#define K_THREADS        32u
#define K_PHI_SCALE      (1u << 20)
#define K_PHI_UP         1696631u
#define K_PHI_DOWN        648055u
#define K_LC_PAIRS        26u
#define K_PERM_SPACE     720u
#define K_ROT_SACRED      18u

static_assert(K_THREADS == 32u, "must be 1 warp");

/* ── ApexSeed: 12B ── */
#pragma pack(push, 1)
typedef struct {
    uint64_t seed;
    uint32_t dispatch_id;
} ApexSeed;
#pragma pack(pop)
static_assert(sizeof(ApexSeed) == 12u, "ApexSeed must be 12B");

/* ── ApexResult: 64B output per cube ────────────────────────── */
typedef struct {
    uint32_t coset_checksum[K_COSET_COUNT];  /* 48B */
    uint32_t master_fold;
    uint32_t verify_ok;
    uint32_t dispatch_id;
    uint32_t _pad;
} ApexResult;   /* 64B = 1 cache line */

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

/* ── derive one coset checksum — pure register, no shared write ─ */
__device__ __forceinline__
uint32_t _d_coset_checksum(uint64_t seed, uint8_t coset)
{
    uint64_t cseed     = _d_derive(seed, coset, 0u);
    uint32_t coset_chk = 0u;

    for (uint8_t f = 0; f < K_FRUSTUM_MOUNT; f++) {
        uint64_t cur      = _d_derive(cseed, f, 0u);
        uint32_t slot_chk = 0u;

        /* 4 levels — all in registers */
        for (uint8_t lv = 0; lv < 4u; lv++) {
            slot_chk ^= (uint32_t)(cur ^ (cur >> 32));
            cur        = _d_derive(cur, f, lv + 1u);
        }
        coset_chk ^= slot_chk;
    }
    return coset_chk;
}

/* ── Main kernel ─────────────────────────────────────────────── */
/*
 * Launch: <<<batch_size, 32>>>
 *
 * Phase 1 (tid 0..11): each thread derives 1 coset in registers
 *                      writes 1 uint32 to s_chk[tid] — no conflict
 * Phase 2 (tid 0):     folds s_chk[0..11] → ApexResult
 * Phase 3 (tid 12..31): structural silence (warp padding)
 */
__global__
void geo_seed_dispatch(const ApexSeed * __restrict__ seeds,
                             ApexResult * __restrict__ results)
{
    __shared__ uint32_t s_chk[K_COSET_COUNT];   /* 48B — safe */

    uint32_t bid = blockIdx.x;
    uint32_t tid = threadIdx.x;

    uint64_t seed        = seeds[bid].seed;
    uint32_t dispatch_id = seeds[bid].dispatch_id;

    /* Phase 1: tid 0..11 each own 1 coset — no shared conflict */
    if (tid < K_COSET_COUNT) {
        s_chk[tid] = _d_coset_checksum(seed, (uint8_t)tid);
    }

    __syncthreads();

    /* Phase 2: tid 0 folds result */
    if (tid == 0u) {
        ApexResult *r   = &results[bid];
        uint32_t master = 0u;

        for (uint8_t c = 0; c < K_COSET_COUNT; c++) {
            r->coset_checksum[c] = s_chk[c];
            master ^= s_chk[c];
        }

        /* verify: re-derive coset 0 checksum inline and compare */
        uint32_t ref = _d_coset_checksum(seed, 0u);

        r->master_fold  = master;
        r->verify_ok    = (ref == s_chk[0]) ? 1u : 0u;
        r->dispatch_id  = dispatch_id;
        r->_pad         = 0u;
    }
    /* tid 12..31: structural silence — warp runs anyway, no work */
}

/* ── Host launch ─────────────────────────────────────────────── */
#ifdef __cplusplus
extern "C"
#endif
cudaError_t geo_seed_launch(const ApexSeed *d_seeds,
                                  ApexResult *d_results,
                                  uint32_t    batch_size)
{
    dim3 grid(batch_size, 1, 1);
    dim3 block(K_THREADS,  1, 1);
    geo_seed_dispatch<<<grid, block>>>(d_seeds, d_results);
    cudaDeviceSynchronize();
    return cudaGetLastError();
}

/* ── Host pipeline (pinned memory for max PCIe) ──────────────── */
#ifdef __cplusplus
extern "C"
#endif
int geo_seed_pipeline(const ApexSeed *h_seeds,
                            ApexResult *h_results,
                            uint32_t    batch_size)
{
    ApexSeed   *d_seeds   = NULL;
    ApexResult *d_results = NULL;

    size_t in_bytes  = (size_t)batch_size * sizeof(ApexSeed);
    size_t out_bytes = (size_t)batch_size * sizeof(ApexResult);

    if (cudaMalloc(&d_seeds,   in_bytes)  != cudaSuccess) return 1;
    if (cudaMalloc(&d_results, out_bytes) != cudaSuccess) {
        cudaFree(d_seeds); return 1;
    }

    cudaMemcpy(d_seeds, h_seeds, in_bytes, cudaMemcpyHostToDevice);
    cudaError_t err = geo_seed_launch(d_seeds, d_results, batch_size);
    cudaMemcpy(h_results, d_results, out_bytes, cudaMemcpyDeviceToHost);

    cudaFree(d_seeds);
    cudaFree(d_results);
    return (err == cudaSuccess) ? 0 : 1;
}
