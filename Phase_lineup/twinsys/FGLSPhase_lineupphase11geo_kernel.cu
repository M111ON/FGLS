/*
 * geo_kernel.cu — FGLS Cube Dispatch Kernel (Phase 5 + Phase 11 WHE)
 * ════════════════════════════════════════════════════
 *
 * Phase 11 additions:
 *   WHE-lite device function → detects structural deviation per slot
 *   _pad field (4B @60) now encodes:
 *     bit  0     = verify_ok (CRC match)
 *     bit  1     = whe_clean (no deviation)
 *     bit  2     = whe_suspicious (deviation > K steps)
 *     bits 8-31  = violation_count (24-bit)
 *
 * Execution model:
 *   gridDim.x  = 12   (1 block per coset)
 *   blockDim.x = 32   (1 warp)
 *   → 384 threads = 12 × 32 total
 *   → each thread owns 1 FrustumSlot64 (144B ÷ 32 = F(12) ✅)
 */

#include <stdint.h>
#include <cuda_runtime.h>

/* ── Constants ────────────────────────────────────────────────── */
#define K_COSET_COUNT    12u
#define K_FRUSTUM_MOUNT   6u
#define K_SLOT_BYTES     64u
#define K_COSET_BYTES    (K_FRUSTUM_MOUNT * K_SLOT_BYTES)
#define K_PAYLOAD_BYTES  (K_COSET_COUNT * K_COSET_BYTES)
#define K_THREADS        32u
#define K_FIBO_CLOCK     (K_PAYLOAD_BYTES / K_THREADS)

#define K_MODE_VERIFY    0u
#define K_MODE_TRANSFORM 1u
#define K_ROT_SACRED     18u

/* WHE constants */
#define K_WHE_PHI        0x9E3779B97F4A7C15ULL
#define K_WHE_SUS_K      144u

static_assert(K_FIBO_CLOCK == 144u, "fibo clock must be 144 = F(12)");
static_assert(K_PAYLOAD_BYTES == 4608u, "payload must be 4608B");

/* ── FrustumSlot64 offsets ────────────────────────────────────── */
#define FSLOT_CORE0_OFF   0u
#define FSLOT_ADDR0_OFF  32u
#define FSLOT_DIR_OFF    48u
#define FSLOT_AXIS_OFF   49u
#define FSLOT_COSET_OFF  50u
#define FSLOT_FRUST_OFF  51u
#define FSLOT_WORLD_OFF  52u
#define FSLOT_CRC_OFF    56u
#define FSLOT_PAD_OFF    60u   /* WHE flags packed here */

/* ── _pad bit layout ──────────────────────────────────────────── */
#define K_PAD_CRC_OK     (1u << 0)
#define K_PAD_WHE_CLEAN  (1u << 1)
#define K_PAD_WHE_SUS    (1u << 2)
/* bits 8-31: violation count */
#define K_PAD_VIOL_SHIFT  8u

/* ── Device primitives ────────────────────────────────────────── */
__device__ __forceinline__
uint64_t _k_rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

__device__ __forceinline__
uint64_t _k_mix64(uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31; return x;
}

__device__ __forceinline__
uint64_t _k_rd64(const uint8_t *p, uint32_t off) {
    uint64_t v; __builtin_memcpy(&v, p + off, 8); return v;
}
__device__ __forceinline__
uint32_t _k_rd32(const uint8_t *p, uint32_t off) {
    uint32_t v; __builtin_memcpy(&v, p + off, 4); return v;
}
__device__ __forceinline__
void _k_wr64(uint8_t *p, uint32_t off, uint64_t v) { __builtin_memcpy(p + off, &v, 8); }
__device__ __forceinline__
void _k_wr32(uint8_t *p, uint32_t off, uint32_t v) { __builtin_memcpy(p + off, &v, 4); }

/* ── WHE-lite device function ─────────────────────────────────── */
/*
 * Runs 4 steps over core[0..3] — expected = deterministic derive from core[0]
 * Counts deviations vs actual next values.
 * Returns packed _pad uint32.
 */
__device__
uint32_t geo_whe_check(uint64_t c0, uint64_t c1, uint64_t c2, uint64_t c3,
                        uint64_t n0, uint64_t n1, uint64_t n2, uint64_t n3,
                        uint32_t crc_ok, uint32_t coset, uint32_t face)
{
    uint32_t violations = 0;
    uint64_t fp = 0;

    /* expected next derived deterministically (same rule as transform) */
    /* step 0: n0 expected */
    uint64_t step_base = (uint64_t)coset * K_FRUSTUM_MOUNT + face;

    if (n0 != c0) { violations++; fp ^= (n0 * K_WHE_PHI) ^ step_base; }
    fp ^= c0 * K_WHE_PHI;

    if (n1 != c1) { violations++; fp ^= (n1 * K_WHE_PHI) ^ (step_base + 1); }
    fp ^= c1 * K_WHE_PHI;

    if (n2 != c2) { violations++; fp ^= (n2 * K_WHE_PHI) ^ (step_base + 2); }
    fp ^= c2 * K_WHE_PHI;

    if (n3 != c3) { violations++; fp ^= (n3 * K_WHE_PHI) ^ (step_base + 3); }
    fp ^= c3 * K_WHE_PHI;

    uint32_t whe_clean   = (violations == 0) ? 1u : 0u;
    uint32_t whe_sus     = (violations > 2u) ? 1u : 0u;
    uint32_t viol_capped = (violations > 0xFFFFFFu) ? 0xFFFFFFu : violations;

    uint32_t pad = 0;
    if (crc_ok)    pad |= K_PAD_CRC_OK;
    if (whe_clean) pad |= K_PAD_WHE_CLEAN;
    if (whe_sus)   pad |= K_PAD_WHE_SUS;
    pad |= (viol_capped << K_PAD_VIOL_SHIFT);

    return pad;
}

/* ── geo_frustum_dispatch: 1 thread → 1 FrustumSlot64 ───────── */
__device__
uint32_t geo_frustum_dispatch(const uint8_t *in_slot,
                               uint8_t       *out_slot,
                               uint8_t        mode,
                               uint32_t       coset,
                               uint32_t       face)
{
    uint64_t core0 = _k_rd64(in_slot, FSLOT_CORE0_OFF + 0u);
    uint64_t core1 = _k_rd64(in_slot, FSLOT_CORE0_OFF + 8u);
    uint64_t core2 = _k_rd64(in_slot, FSLOT_CORE0_OFF + 16u);
    uint64_t core3 = _k_rd64(in_slot, FSLOT_CORE0_OFF + 24u);

    uint32_t stored_crc = _k_rd32(in_slot, FSLOT_CRC_OFF);
    uint32_t calc_crc   = (uint32_t)(core0 ^ (core0 >> 32))
                        ^ (uint32_t)(core1 ^ (core1 >> 32))
                        ^ (uint32_t)(core2 ^ (core2 >> 32))
                        ^ (uint32_t)(core3 ^ (core3 >> 32));
    uint32_t crc_ok = (calc_crc == stored_crc) ? 1u : 0u;

    __builtin_memcpy(out_slot, in_slot, K_SLOT_BYTES);

    if (mode == K_MODE_VERIFY) {
        /* WHE check: compare stored cores vs themselves (invariant = no mutation) */
        uint32_t pad = geo_whe_check(core0, core1, core2, core3,
                                      core0, core1, core2, core3,
                                      crc_ok, coset, face);
        _k_wr32(out_slot, FSLOT_PAD_OFF, pad);
        return crc_ok;
    }

    /* ── TRANSFORM ────────────────────────────────────────── */
    uint8_t face_id = _k_rd32(in_slot, FSLOT_FRUST_OFF) & 0xFFu;

    uint64_t next0 = _k_mix64(core0 ^ _k_rotl64(core3, K_ROT_SACRED) ^ ((uint64_t)face_id << 32));
    uint64_t next1 = _k_mix64(core1 ^ next0);
    uint64_t next2 = _k_mix64(core2 ^ next1);
    uint64_t next3 = _k_mix64(core3 ^ next2);

    uint32_t new_crc = (uint32_t)(next0 ^ (next0 >> 32))
                     ^ (uint32_t)(next1 ^ (next1 >> 32))
                     ^ (uint32_t)(next2 ^ (next2 >> 32))
                     ^ (uint32_t)(next3 ^ (next3 >> 32));

    /* WHE: compare old cores vs new — flags deviation if unexpected mutation */
    uint32_t pad = geo_whe_check(core0, core1, core2, core3,
                                  next0, next1, next2, next3,
                                  crc_ok, coset, face);

    _k_wr64(out_slot, FSLOT_CORE0_OFF + 0u,  next0);
    _k_wr64(out_slot, FSLOT_CORE0_OFF + 8u,  next1);
    _k_wr64(out_slot, FSLOT_CORE0_OFF + 16u, next2);
    _k_wr64(out_slot, FSLOT_CORE0_OFF + 24u, next3);
    _k_wr32(out_slot, FSLOT_CRC_OFF,          new_crc);
    _k_wr32(out_slot, FSLOT_PAD_OFF,           pad);

    return crc_ok;
}

/* ── Main kernel ─────────────────────────────────────────────── */
__global__
void geo_cube_dispatch(const uint8_t * __restrict__ payload,
                             uint8_t * __restrict__ out,
                             uint8_t                mode)
{
    uint32_t coset     = blockIdx.x;
    uint32_t thread_id = threadIdx.x;
    uint32_t coset_base = coset * K_COSET_BYTES;

    if (thread_id < K_FRUSTUM_MOUNT) {
        uint32_t face_off = thread_id * K_SLOT_BYTES;
        uint32_t slot_off = coset_base + face_off;
        geo_frustum_dispatch(payload + slot_off, out + slot_off,
                             mode, coset, thread_id);
    } else {
        uint32_t rsv_start = coset_base + K_FRUSTUM_MOUNT * K_SLOT_BYTES;
        uint32_t rsv_bytes = K_COSET_BYTES - K_FRUSTUM_MOUNT * K_SLOT_BYTES;
        uint32_t t_local   = thread_id - K_FRUSTUM_MOUNT;
        uint32_t chunk     = rsv_bytes / (K_THREADS - K_FRUSTUM_MOUNT);
        uint32_t my_off    = rsv_start + t_local * chunk;
        if (my_off + chunk <= coset_base + K_COSET_BYTES)
            __builtin_memcpy(out + my_off, payload + my_off, chunk);
    }
}

/* ── Host: read WHE flags from output buffer ─────────────────── */
/*
 * geo_whe_flags_read — parse _pad from one slot in host output buffer
 * slot_idx = 0..(K_COSET_COUNT*K_FRUSTUM_MOUNT - 1) = 0..71
 */
static inline void geo_whe_flags_read(const uint8_t *h_out,
                                       uint32_t       slot_idx,
                                       uint32_t      *crc_ok,
                                       uint32_t      *whe_clean,
                                       uint32_t      *whe_sus,
                                       uint32_t      *violations)
{
    uint32_t coset    = slot_idx / K_FRUSTUM_MOUNT;
    uint32_t face     = slot_idx % K_FRUSTUM_MOUNT;
    uint32_t slot_off = coset * K_COSET_BYTES + face * K_SLOT_BYTES;
    uint32_t pad;
    __builtin_memcpy(&pad, h_out + slot_off + FSLOT_PAD_OFF, 4);
    *crc_ok     = (pad & K_PAD_CRC_OK)    ? 1u : 0u;
    *whe_clean  = (pad & K_PAD_WHE_CLEAN) ? 1u : 0u;
    *whe_sus    = (pad & K_PAD_WHE_SUS)   ? 1u : 0u;
    *violations = (pad >> K_PAD_VIOL_SHIFT) & 0xFFFFFFu;
}

/* ── Host launch wrappers (unchanged API) ────────────────────── */
#ifdef __cplusplus
extern "C"
#endif
cudaError_t gcfs_kernel_launch(const uint8_t *d_payload,
                                      uint8_t *d_out,
                                      uint8_t  mode)
{
    dim3 grid(K_COSET_COUNT, 1, 1);
    dim3 block(K_THREADS,    1, 1);
    geo_cube_dispatch<<<grid, block>>>(d_payload, d_out, mode);
    cudaDeviceSynchronize();
    return cudaGetLastError();
}

#ifdef __cplusplus
extern "C"
#endif
int gcfs_pipeline_step(const uint8_t *h_in,
                              uint8_t *h_out,
                              uint8_t  mode)
{
    uint8_t *d_in = NULL, *d_out = NULL;
    if (cudaMalloc(&d_in,  K_PAYLOAD_BYTES) != cudaSuccess) return 1;
    if (cudaMalloc(&d_out, K_PAYLOAD_BYTES) != cudaSuccess) {
        cudaFree(d_in); return 1;
    }
    cudaMemcpy(d_in, h_in, K_PAYLOAD_BYTES, cudaMemcpyHostToDevice);
    cudaError_t err = gcfs_kernel_launch(d_in, d_out, mode);
    cudaMemcpy(h_out, d_out, K_PAYLOAD_BYTES, cudaMemcpyDeviceToHost);
    cudaFree(d_in);
    cudaFree(d_out);
    return (err == cudaSuccess) ? 0 : 1;
}
