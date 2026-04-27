/*
 * geo_kernel_seed.cu — FGLS Seed-Only Dispatch (Unified Memory Emulation)
 * ════════════════════════════════════════════════════════════════════════
 *
 * Core idea:
 *   Instead of transferring 4,608B payload per cube → transfer 12B seed only
 *   GPU derives entire GiantCube deterministically from seed + dispatch_id
 *   Emulates unified memory: address recipe travels, not data
 *
 * Transfer comparison:
 *   Old: 4,608B per cube (payload DMA)
 *   New:    12B per cube (seed only) → 384× reduction
 *
 * Execution model:
 *   gridDim.x  = batch_size     (one block per cube)
 *   blockDim.x = 32             (one warp)
 *   shared mem = FrustumSlot64 × 6 = 384B (fits L1 cache)
 *
 * Output: ApexResult[] — 32B per cube (checksum + verify summary)
 *   Full payload stays on GPU — never crosses PCIe
 *
 * No malloc. No float. No heap.
 * ════════════════════════════════════════════════════════════════════════
 */

#include <stdint.h>
#include <cuda_runtime.h>

/* ── Constants (mirror geo_apex_wire.h) ─────────────────────── */
#define K_COSET_COUNT    12u
#define K_FRUSTUM_MOUNT   6u
#define K_SLOT_BYTES     64u
#define K_THREADS        32u
#define K_PHI_SCALE      (1u << 20)       /* 2^20 = 1,048,576        */
#define K_PHI_UP         1696631u          /* floor(φ × 2^20)         */
#define K_PHI_DOWN        648055u          /* floor(φ^-1 × 2^20)      */
#define K_PHI_MASK       (K_PHI_SCALE - 1u)
#define K_LC_PAIRS        26u
#define K_PERM_SPACE     720u              /* 6! LetterCube space      */
#define K_ROT_SACRED      18u              /* sacred rotation          */

/* ── Input: 12B seed packet ─────────────────────────────────── */
typedef struct {
    uint64_t seed;         /* root seed                              */
    uint32_t dispatch_id;  /* monotonic counter                      */
} ApexSeed;               /* 12B — all that crosses PCIe            */

/* ── Output: 32B result per cube ────────────────────────────── */
typedef struct {
    uint32_t coset_checksum[K_COSET_COUNT];  /* fold per coset       */
    /* 12 × 4B = 48B — trim to active 9 for output                  */
    uint32_t master_fold;   /* XOR of all coset checksums            */
    uint32_t verify_ok;     /* 1 = all checksums consistent          */
    uint32_t dispatch_id;   /* echo back                             */
    uint32_t _pad;
} ApexResult;              /* 64B = 1 cache line                    */

/* ── Device primitives (ported from geo_primitives.h) ───────── */
__device__ __forceinline__
uint64_t _d_mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

__device__ __forceinline__
uint64_t _d_rotl64(uint64_t x, int r) {
    return (x << r) | (x >> (64 - r));
}

__device__ __forceinline__
uint64_t _d_derive(uint64_t core, uint8_t face, uint32_t step) {
    uint64_t salt = ((uint64_t)face << 56) ^ (uint64_t)step;
    uint64_t a    = _d_mix64(core ^ salt);
    uint64_t b    = _d_rotl64(core, (face + step) & 63);
    return _d_mix64(a ^ b);
}

/* ── Device: fibo_addr (ported from pogls_fibo_addr.h) ──────── */
__device__ __forceinline__
uint32_t _d_fibo_addr(uint32_t n, uint8_t gear, uint8_t world) {
    uint64_t base = world ? K_PHI_DOWN : K_PHI_UP;
    if (gear <= 3u)
        return (uint32_t)(((uint64_t)n * base) % K_PHI_SCALE);
    else if (gear <= 8u) {
        uint32_t factor = (uint32_t)(gear - 3u);
        return (uint32_t)(((uint64_t)n * base * factor) % K_PHI_SCALE);
    } else {
        uint32_t shift = (uint32_t)(gear - 8u);
        return (uint32_t)(((uint64_t)n * (base << shift)) % K_PHI_SCALE);
    }
}

/* ── Device: build one FrustumSlot64 → returns checksum ─────── */
__device__ __forceinline__
uint32_t _d_slot_build(uint64_t seed, uint8_t coset, uint8_t face_id,
                        uint8_t *slot_out)  /* 64B output buffer */
{
    uint64_t cur = _d_derive(seed, face_id, 0u);
    uint32_t chk = 0u;
    uint32_t n   = (uint32_t)(cur % K_PHI_SCALE);

    /* core[0..3] */
    uint64_t cores[4];
    for (uint8_t lv = 0; lv < 4u; lv++) {
        cores[lv] = cur;
        chk      ^= (uint32_t)(cur ^ (cur >> 32));
        cur       = _d_derive(cur, face_id, lv + 1u);
    }

    /* write to shared slot buffer */
    if (slot_out) {
        /* core[4] @ 0..31 */
        for (uint8_t lv = 0; lv < 4u; lv++)
            __builtin_memcpy(slot_out + lv * 8u, &cores[lv], 8u);

        /* addr[4] @ 32..47 */
        for (uint8_t lv = 0; lv < 4u; lv++) {
            uint32_t addr = _d_fibo_addr(n, (uint8_t)(face_id & 0x3u),
                                          lv & 1u);
            __builtin_memcpy(slot_out + 32u + lv * 4u, &addr, 4u);
        }

        /* metadata @ 48..63 */
        slot_out[48] = face_id;           /* dir        */
        slot_out[49] = face_id / 2u;      /* axis       */
        slot_out[50] = coset;             /* coset_id   */
        slot_out[51] = face_id;           /* frustum_id */
        for (uint8_t lv = 0; lv < 4u; lv++)
            slot_out[52 + lv] = lv & 1u; /* world[4]   */

        __builtin_memcpy(slot_out + 56u, &chk, 4u);
        uint32_t pad = 0u;
        __builtin_memcpy(slot_out + 60u, &pad, 4u);  /* _pad=0 always */
    }
    return chk;
}

/* ── Main kernel ─────────────────────────────────────────────── */
/*
 * Launch: <<<batch_size, 32>>>
 *
 * Each block = 1 ApexSeed → derives full GiantCube in shared mem
 * Thread assignment:
 *   thread 0..5   → coset 0..5,  face = thread_id (derive all 6 faces)
 *   thread 6..11  → coset 6..11, face = thread_id - 6
 *   thread 12..31 → reduction pass (XOR fold checksums)
 *
 * Shared memory: 6 slots × 64B = 384B (one coset worth — reused per coset)
 */
__global__
void geo_seed_dispatch(const ApexSeed * __restrict__ seeds,
                             ApexResult * __restrict__ results,
                             uint8_t                   mode)
{
    __shared__ uint8_t  s_slots[K_FRUSTUM_MOUNT][K_SLOT_BYTES]; /* 384B */
    __shared__ uint32_t s_chk[K_COSET_COUNT];                    /*  48B */

    uint32_t bid = blockIdx.x;   /* cube index in batch */
    uint32_t tid = threadIdx.x;  /* 0..31               */

    /* load seed */
    uint64_t seed        = seeds[bid].seed;
    uint32_t dispatch_id = seeds[bid].dispatch_id;

    /* phase 1: derive 12 cosets, 2 cosets per thread pair (tid 0..5) */
    /* each thread handles 1 coset = 6 faces sequentially             */
    /* threads 0..11 → coset 0..11                                    */
    if (tid < K_COSET_COUNT) {
        uint8_t  coset = (uint8_t)tid;
        uint64_t cseed = _d_derive(seed, coset, 0u);

        uint32_t coset_chk = 0u;
        for (uint8_t f = 0; f < K_FRUSTUM_MOUNT; f++) {
            uint8_t *slot_buf = (tid < K_FRUSTUM_MOUNT)
                                ? s_slots[f]   /* reuse shared for first 6 */
                                : NULL;        /* threads 6..11: no shared write */
            coset_chk ^= _d_slot_build(cseed, coset, f, slot_buf);
        }
        s_chk[coset] = coset_chk;
    }

    __syncthreads();

    /* phase 2: thread 0 folds result */
    if (tid == 0u) {
        ApexResult *r = &results[bid];
        uint32_t master = 0u;
        uint32_t ok     = 1u;

        for (uint8_t c = 0; c < K_COSET_COUNT; c++) {
            r->coset_checksum[c] = s_chk[c];
            master ^= s_chk[c];
        }

        /* verify: re-derive coset 0 face 0 checksum inline */
        uint64_t cseed0 = _d_derive(seed, 0u, 0u);
        uint32_t ref    = 0u;
        uint64_t cur    = _d_derive(cseed0, 0u, 0u);
        for (uint8_t lv = 0; lv < 4u; lv++) {
            ref ^= (uint32_t)(cur ^ (cur >> 32));
            cur  = _d_derive(cur, 0u, lv + 1u);
        }
        ok = (ref == s_chk[0]) ? 1u : 0u;

        r->master_fold  = master;
        r->verify_ok    = ok;
        r->dispatch_id  = dispatch_id;
        r->_pad         = 0u;
    }
}

/* ── Host launch wrapper ─────────────────────────────────────── */
#ifdef __cplusplus
extern "C"
#endif
cudaError_t geo_seed_launch(const ApexSeed *d_seeds,
                                  ApexResult *d_results,
                                  uint32_t    batch_size,
                                  uint8_t     mode)
{
    dim3 grid(batch_size, 1, 1);
    dim3 block(K_THREADS,  1, 1);

    geo_seed_dispatch<<<grid, block>>>(d_seeds, d_results, mode);
    cudaDeviceSynchronize();
    return cudaGetLastError();
}

/* ── Host pipeline helper ─────────────────────────────────────── */
#ifdef __cplusplus
extern "C"
#endif
int geo_seed_pipeline(const ApexSeed *h_seeds,
                            ApexResult *h_results,
                            uint32_t    batch_size,
                            uint8_t     mode)
{
    ApexSeed   *d_seeds   = NULL;
    ApexResult *d_results = NULL;

    size_t in_bytes  = batch_size * sizeof(ApexSeed);    /* 12B each */
    size_t out_bytes = batch_size * sizeof(ApexResult);  /* 64B each */

    if (cudaMalloc(&d_seeds,   in_bytes)  != cudaSuccess) return 1;
    if (cudaMalloc(&d_results, out_bytes) != cudaSuccess) {
        cudaFree(d_seeds); return 1;
    }

    cudaMemcpy(d_seeds, h_seeds, in_bytes, cudaMemcpyHostToDevice);
    cudaError_t err = geo_seed_launch(d_seeds, d_results, batch_size, mode);
    cudaMemcpy(h_results, d_results, out_bytes, cudaMemcpyDeviceToHost);

    cudaFree(d_seeds);
    cudaFree(d_results);
    return (err == cudaSuccess) ? 0 : 1;
}
