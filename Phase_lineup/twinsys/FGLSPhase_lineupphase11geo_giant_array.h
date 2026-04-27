/*
 * geo_giant_array.h — FGLS Giant Cube Array (12 coset slots)
 * ═══════════════════════════════════════════════════════════
 *
 * 12 GiantCubes — one per LetterCube coset (720/60 = 12)
 * Each GiantCube = 6 frustum mounts × 64B slot = 384B
 * Full array = 12 × 384 = 4,608B = APEX_DISPATCH_BYTES ✅
 *
 * Addressing:
 *   apex → lc_route_coset() → GiantCube index 0..11
 *   GiantCube → frustum 0..5 → 64B payload
 *
 * GPU dispatch:
 *   gridDim.x  = 12  (one block per coset)
 *   blockDim.x = 32  (one warp per block)
 *   → 144 threads total = F(12) = 1 fibo clock ✅
 *
 * No malloc, no float, no heap.
 * ═══════════════════════════════════════════════════════════
 */

#ifndef GEO_GIANT_ARRAY_H
#define GEO_GIANT_ARRAY_H

#include <stdint.h>
#include <string.h>
#include "geo_apex_wire.h"
#include "geo_primitives.h"
#include "pogls_fibo_addr.h"

/* ── FrustumSlot64: 64B payload per frustum face ────────────── */
/* 4 × 16B GpuFrustumSlot packed into one cache line           */
typedef struct {
    uint64_t core[4];      /* L0..L3 level cores                   */
    uint32_t addr[4];      /* fibo addr per level                  */
    uint8_t  dir;          /* FRUSTUM_DIR_*                        */
    uint8_t  axis;         /* 0=X 1=Y 2=Z                          */
    uint8_t  coset;        /* parent coset 0..11                   */
    uint8_t  frustum_id;   /* 0..5                                 */
    uint8_t  world[4];     /* per-level world flag                 */
    uint32_t checksum;     /* XOR fold of core[0..3]               */
    uint32_t _pad;
} FrustumSlot64;           /* 64B exactly ✅                        */

typedef char _slot64_size_assert[
    (sizeof(FrustumSlot64) == 64u) ? 1 : -1
];

/* ── GiantCube: one coset unit ───────────────────────────────── */
typedef struct {
    FrustumSlot64  faces[APEX_FRUSTUM_MOUNT];  /* 6 × 64B = 384B   */
    uint64_t       seed;                        /* init seed         */
    uint8_t        coset;                       /* 0..11             */
    uint8_t        mounted;                     /* faces populated   */
    uint16_t       _pad;
    uint32_t       master_core_lo;              /* fold of all cores */
} GiantCube;               /* 384 + 16 = 400B — pad to 512B in GPU  */

/* ── GiantArray: full 12-slot dispatch unit ──────────────────── */
typedef struct {
    GiantCube cubes[APEX_COSET_COUNT];  /* 12 × GiantCube            */
    uint32_t  active_mask;              /* bitmask: which cosets live */
    uint32_t  dispatch_id;             /* monotonic dispatch counter  */
} GiantArray;

/* ── slot64_build: derive FrustumSlot64 from seed + face ────── */
static inline void slot64_build(FrustumSlot64 *s,
                                 uint64_t seed,
                                 uint8_t  coset,
                                 uint8_t  face_id)
{
    s->coset      = coset;
    s->frustum_id = face_id;
    s->axis       = face_id / 2u;         /* 0,0,1,1,2,2 → X,Y,Z   */
    s->dir        = face_id;              /* maps to FRUSTUM_DIR_*   */

    uint64_t cur  = derive_next_core(seed, face_id, 0u);
    uint32_t chk  = 0u;

    for (uint8_t lv = 0; lv < 4u; lv++) {
        s->core[lv]  = cur;
        s->world[lv] = lv & 1u;          /* alternate A/B per level */
        s->addr[lv]  = fibo_addr(lv, (uint8_t)(face_id & 0x3u), s->world[lv]);
        chk         ^= (uint32_t)(cur ^ (cur >> 32));
        cur          = derive_next_core(cur, face_id, lv + 1u);
    }
    s->checksum = chk;
    s->_pad     = 0u;
}

/* ── giant_cube_init ─────────────────────────────────────────── */
static inline void giant_cube_init(GiantCube *gc,
                                    uint64_t   seed,
                                    uint8_t    coset)
{
    gc->seed            = seed;
    gc->coset           = coset;
    gc->mounted         = 0u;
    gc->master_core_lo  = 0u;

    uint32_t fold = 0u;
    for (uint8_t f = 0; f < APEX_FRUSTUM_MOUNT; f++) {
        slot64_build(&gc->faces[f], seed, coset, f);
        fold ^= gc->faces[f].checksum;
        gc->mounted++;
    }
    gc->master_core_lo = fold;
}

/* ── giant_array_init: build all 12 GiantCubes from root seed ── */
static inline void giant_array_init(GiantArray *ga, uint64_t root_seed)
{
    ga->active_mask  = 0u;
    ga->dispatch_id  = 0u;

    for (uint8_t c = 0; c < APEX_COSET_COUNT; c++) {
        uint64_t cseed = derive_next_core(root_seed, c, 0u);
        giant_cube_init(&ga->cubes[c], cseed, c);
        ga->active_mask |= (1u << c);
    }
}

/* ── giant_array_dispatch: ApexWire → GiantCube + offset ────── */
/*
 * Returns pointer to GiantCube and sets *byte_offset to the
 * flat byte offset within the 4608B dispatch payload.
 */
static inline GiantCube *giant_array_dispatch(GiantArray    *ga,
                                               const ApexWire *w,
                                               uint32_t       *byte_offset)
{
    uint8_t  coset  = lc_route_coset(w);
    uint8_t  face   = (uint8_t)((w->hilbert_a ^ w->hilbert_b) % APEX_FRUSTUM_MOUNT);
    *byte_offset    = apex_dispatch_offset(coset, face);
    return &ga->cubes[coset];
}

/* ── giant_array_flat_pack: serialize → 4608B buffer ────────── */
/*
 * Flat layout for DMA to GPU:
 *   [coset0: face0..5 × 64B][coset1: ...] ... [coset11: ...]
 *   = 12 × 6 × 64 = 4,608B ✅
 */
static inline void giant_array_flat_pack(const GiantArray *ga,
                                          uint8_t out[APEX_DISPATCH_BYTES])
{
    for (uint8_t c = 0; c < APEX_COSET_COUNT; c++) {
        const GiantCube *gc = &ga->cubes[c];
        for (uint8_t f = 0; f < APEX_FRUSTUM_MOUNT; f++) {
            uint32_t off = apex_dispatch_offset(c, f);
            memcpy(out + off, &gc->faces[f], APEX_SLOT_BYTES);
        }
    }
}

/* ── Compile-time size checks ────────────────────────────────── */
typedef char _giant_dispatch_assert[
    (APEX_COSET_COUNT * APEX_FRUSTUM_MOUNT * APEX_SLOT_BYTES == 4608u) ? 1 : -1
];
typedef char _fibo_clock_assert[
    (APEX_FIBO_CLOCK == 144u) ? 1 : -1
];

#endif /* GEO_GIANT_ARRAY_H */
