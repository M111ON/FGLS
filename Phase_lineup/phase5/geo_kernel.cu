/*
 * geo_kernel.cu — FGLS Cube Dispatch Kernel (Phase 5)
 * ════════════════════════════════════════════════════
 *
 * Execution model:
 *   gridDim.x  = 12   (1 block per coset)
 *   blockDim.x = 32   (1 warp)
 *   → 384 threads = 12 × 32 total
 *   → each thread owns 1 FrustumSlot64 (144B ÷ 32 = F(12) ✅)
 *
 * I/O:
 *   input  = uint8_t* payload  (4608B flat)
 *   output = uint8_t* out      (4608B flat)
 *   mode   = 0 → verify only
 *            1 → verify + derive next core + write back
 *
 * Thread → slot mapping:
 *   coset      = blockIdx.x          (0..11)
 *   thread_id  = threadIdx.x         (0..31)
 *   face       = thread_id / 8        (0..5)   — 32 threads ÷ 6 faces = ~5.3
 *   sub        = thread_id % 8        (level selector within face)
 *
 *   byte offset = coset * 384 + face * 64
 *
 * FrustumSlot64 layout (64B):
 *   core[4]    uint64 × 4 = 32B   @0
 *   addr[4]    uint32 × 4 = 16B   @32
 *   dir        uint8        1B    @48
 *   axis       uint8        1B    @49
 *   coset_id   uint8        1B    @50
 *   frustum_id uint8        1B    @51
 *   world[4]   uint8 × 4  = 4B    @52
 *   checksum   uint32       4B    @56
 *   _pad       uint32       4B    @60
 *
 * Derive rule (1 step, sacred):
 *   next = mix64(core[0] ^ rot64(core[3], 18) ^ ((uint64)face_id << 32))
 *   checksum_new = XOR fold of all core[0..3] after update
 *
 * No malloc. No float. No host structs.
 * ════════════════════════════════════════════════════
 */

#include <stdint.h>
#include <cuda_runtime.h>

/* ── Constants (mirror geo_apex_wire.h — no host header in kernel) */
#define K_COSET_COUNT   12u
#define K_FRUSTUM_MOUNT  6u
#define K_SLOT_BYTES    64u
#define K_COSET_BYTES   (K_FRUSTUM_MOUNT * K_SLOT_BYTES)  /* 384B */
#define K_PAYLOAD_BYTES (K_COSET_COUNT * K_COSET_BYTES)   /* 4608B */
#define K_THREADS       32u
#define K_FIBO_CLOCK    (K_PAYLOAD_BYTES / K_THREADS)     /* 144 = F(12) ✅ */

#define K_MODE_VERIFY    0u
#define K_MODE_TRANSFORM 1u

/* Sacred rotation */
#define K_ROT_SACRED    18u

/* ── Compile-time checks ──────────────────────────────────────── */
static_assert(K_FIBO_CLOCK == 144u, "fibo clock must be 144 = F(12)");
static_assert(K_PAYLOAD_BYTES == 4608u, "payload must be 4608B");

/* ── Device primitives (no host dep) ─────────────────────────── */
__device__ __forceinline__
uint64_t _k_rotl64(uint64_t x, int r) {
    return (x << r) | (x >> (64 - r));
}

__device__ __forceinline__
uint64_t _k_mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

/* ── FrustumSlot64 byte offsets (device) ─────────────────────── */
#define FSLOT_CORE0_OFF   0u    /* uint64[4] = 32B */
#define FSLOT_ADDR0_OFF  32u    /* uint32[4] = 16B */
#define FSLOT_DIR_OFF    48u
#define FSLOT_AXIS_OFF   49u
#define FSLOT_COSET_OFF  50u
#define FSLOT_FRUST_OFF  51u
#define FSLOT_WORLD_OFF  52u    /* uint8[4]  =  4B */
#define FSLOT_CRC_OFF    56u    /* uint32    =  4B */
#define FSLOT_PAD_OFF    60u

/* ── Read/write helpers: unaligned-safe via memcpy ───────────── */
__device__ __forceinline__
uint64_t _k_rd64(const uint8_t *p, uint32_t off) {
    uint64_t v; __builtin_memcpy(&v, p + off, 8); return v;
}
__device__ __forceinline__
uint32_t _k_rd32(const uint8_t *p, uint32_t off) {
    uint32_t v; __builtin_memcpy(&v, p + off, 4); return v;
}
__device__ __forceinline__
void _k_wr64(uint8_t *p, uint32_t off, uint64_t v) {
    __builtin_memcpy(p + off, &v, 8);
}
__device__ __forceinline__
void _k_wr32(uint8_t *p, uint32_t off, uint32_t v) {
    __builtin_memcpy(p + off, &v, 4);
}

/* ── geo_frustum_dispatch: 1 thread → 1 FrustumSlot64 ───────── */
__device__
uint32_t geo_frustum_dispatch(const uint8_t *in_slot,
                               uint8_t       *out_slot,
                               uint8_t        mode)
{
    /* read core[0..3] */
    uint64_t core0 = _k_rd64(in_slot, FSLOT_CORE0_OFF + 0u);
    uint64_t core1 = _k_rd64(in_slot, FSLOT_CORE0_OFF + 8u);
    uint64_t core2 = _k_rd64(in_slot, FSLOT_CORE0_OFF + 16u);
    uint64_t core3 = _k_rd64(in_slot, FSLOT_CORE0_OFF + 24u);

    /* stored checksum */
    uint32_t stored_crc = _k_rd32(in_slot, FSLOT_CRC_OFF);

    /* verify: XOR fold of core[0..3] */
    uint32_t calc_crc = (uint32_t)(core0 ^ (core0 >> 32))
                      ^ (uint32_t)(core1 ^ (core1 >> 32))
                      ^ (uint32_t)(core2 ^ (core2 >> 32))
                      ^ (uint32_t)(core3 ^ (core3 >> 32));

    uint32_t verify_ok = (calc_crc == stored_crc) ? 1u : 0u;

    if (mode == K_MODE_VERIFY) {
        /* copy through, flag checksum field with verify result */
        __builtin_memcpy(out_slot, in_slot, K_SLOT_BYTES);
        /* write verify status in _pad field (non-destructive) */
        _k_wr32(out_slot, FSLOT_PAD_OFF, verify_ok);
        return verify_ok;
    }

    /* ── mode = TRANSFORM ──────────────────────────────────── */
    uint8_t face_id = _k_rd32(in_slot, FSLOT_FRUST_OFF) & 0xFFu;

    /* derive next core[0] from core[0..3] + face_id (sacred rot=18) */
    uint64_t next0 = _k_mix64(
        core0 ^ _k_rotl64(core3, K_ROT_SACRED) ^ ((uint64_t)face_id << 32)
    );
    /* cascade: each level rolls forward 1 step */
    uint64_t next1 = _k_mix64(core1 ^ next0);
    uint64_t next2 = _k_mix64(core2 ^ next1);
    uint64_t next3 = _k_mix64(core3 ^ next2);

    /* new checksum */
    uint32_t new_crc = (uint32_t)(next0 ^ (next0 >> 32))
                     ^ (uint32_t)(next1 ^ (next1 >> 32))
                     ^ (uint32_t)(next2 ^ (next2 >> 32))
                     ^ (uint32_t)(next3 ^ (next3 >> 32));

    /* copy full slot → out, then overwrite cores + checksum */
    __builtin_memcpy(out_slot, in_slot, K_SLOT_BYTES);
    _k_wr64(out_slot, FSLOT_CORE0_OFF + 0u,  next0);
    _k_wr64(out_slot, FSLOT_CORE0_OFF + 8u,  next1);
    _k_wr64(out_slot, FSLOT_CORE0_OFF + 16u, next2);
    _k_wr64(out_slot, FSLOT_CORE0_OFF + 24u, next3);
    _k_wr32(out_slot, FSLOT_CRC_OFF,          new_crc);
    _k_wr32(out_slot, FSLOT_PAD_OFF,           verify_ok);

    return verify_ok;
}

/* ── Main kernel ─────────────────────────────────────────────── */
/*
 * Launch: <<<12, 32>>>
 *
 * Thread assignment:
 *   coset     = blockIdx.x          0..11
 *   thread_id = threadIdx.x         0..31
 *
 *   Each block handles 1 coset = 6 faces × 64B = 384B
 *   32 threads cover 384B → each thread = 12B chunk
 *   But 1 FrustumSlot64 = 64B → threads 0..5 map 1:1 to faces
 *   threads 6..31 → reserved slots (no-op, copy only)
 *
 *   face_id = thread_id < 6 ? thread_id : inactive
 */
__global__
void geo_cube_dispatch(const uint8_t * __restrict__ payload,
                             uint8_t * __restrict__ out,
                             uint8_t                mode)
{
    uint32_t coset     = blockIdx.x;   /* 0..11 */
    uint32_t thread_id = threadIdx.x;  /* 0..31 */

    /* base offset for this coset in 4608B flat */
    uint32_t coset_base = coset * K_COSET_BYTES;  /* 0,384,768,...,4224 */

    if (thread_id < K_FRUSTUM_MOUNT) {
        /* active face thread */
        uint32_t face_off = thread_id * K_SLOT_BYTES;  /* 0,64,128,192,256,320 */
        uint32_t slot_off = coset_base + face_off;

        geo_frustum_dispatch(payload + slot_off,
                             out     + slot_off,
                             mode);
    } else {
        /* threads 6..31: padding threads — copy reserved bytes
         * Each thread copies K_SLOT_BYTES/32 portion of remaining space
         * Reserved slots = zero-fill passthrough (structural silence)
         */
        uint32_t rsv_start = coset_base + K_FRUSTUM_MOUNT * K_SLOT_BYTES;
        uint32_t rsv_bytes = K_COSET_BYTES - K_FRUSTUM_MOUNT * K_SLOT_BYTES;
        uint32_t t_local   = thread_id - K_FRUSTUM_MOUNT;   /* 0..25 */
        uint32_t chunk     = rsv_bytes / (K_THREADS - K_FRUSTUM_MOUNT);
        uint32_t my_off    = rsv_start + t_local * chunk;

        /* safety: bounds */
        if (my_off + chunk <= coset_base + K_COSET_BYTES) {
            __builtin_memcpy(out + my_off, payload + my_off, chunk);
        }
    }
}

/* ── Host launch wrapper ─────────────────────────────────────── */
/*
 * gcfs_kernel_launch: call from host after gcfs_write()
 *
 *   d_payload = device pointer to 4608B input
 *   d_out     = device pointer to 4608B output
 *   mode      = K_MODE_VERIFY | K_MODE_TRANSFORM
 *
 * Returns cudaError_t from cudaGetLastError()
 */
#ifdef __cplusplus
extern "C"
#endif
cudaError_t gcfs_kernel_launch(const uint8_t *d_payload,
                                      uint8_t *d_out,
                                      uint8_t  mode)
{
    dim3 grid(K_COSET_COUNT, 1, 1);   /* 12 blocks */
    dim3 block(K_THREADS,    1, 1);   /* 32 threads = 1 warp */

    geo_cube_dispatch<<<grid, block>>>(d_payload, d_out, mode);
    cudaDeviceSynchronize();
    return cudaGetLastError();
}

/* ── Host pipeline helper ─────────────────────────────────────── */
/*
 * gcfs_pipeline_step: alloc + copy + launch + copy back
 * convenience for single-step pipeline on host-side buffer
 *
 * in/out = host pointers (4608B each)
 * Returns 0=ok, 1=cuda error
 */
#ifdef __cplusplus
extern "C"
#endif
int gcfs_pipeline_step(const uint8_t *h_in,
                              uint8_t *h_out,
                              uint8_t  mode)
{
    uint8_t *d_in  = NULL;
    uint8_t *d_out = NULL;

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
