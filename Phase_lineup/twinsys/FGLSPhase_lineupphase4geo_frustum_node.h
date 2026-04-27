/*
 * geo_frustum_node.h — FGLS Frustum Node (Phase 4)
 * ══════════════════════════════════════════════════
 *
 * Frustum = truncated pyramid with Fibo slope scaling
 *
 * Shape analogy:
 *   wide_core ──►[====]──► narrow_core
 *   (data in)   φ slope    (apex end)
 *
 * Fibo levels (gear tiers reused from fibo_addr):
 *   L0 = Core mount point    (1 unit)
 *   L1 = G1 direct  φ¹      (× PHI_UP)
 *   L2 = G2 batch   φ²      (× PHI_UP²)
 *   L3 = G3 blast   φ³      (× PHI_UP³)
 *
 * Direction encoding (3 bits, maps to axis + polarity):
 *   AXIS_X_POS = 0   AXIS_X_NEG = 1
 *   AXIS_Y_POS = 2   AXIS_Y_NEG = 3
 *   AXIS_Z_POS = 4   AXIS_Z_NEG = 5
 */

#ifndef GEO_FRUSTUM_NODE_H
#define GEO_FRUSTUM_NODE_H

#include <stdint.h>
#include "../core/geo_primitives.h"
#include "../core/pogls_fibo_addr.h"
#include "../core/geo_config.h"

/* ── Direction constants ─────────────────────── */
#define FRUSTUM_DIR_X_POS  0u
#define FRUSTUM_DIR_X_NEG  1u
#define FRUSTUM_DIR_Y_POS  2u
#define FRUSTUM_DIR_Y_NEG  3u
#define FRUSTUM_DIR_Z_POS  4u
#define FRUSTUM_DIR_Z_NEG  5u
#define FRUSTUM_DIR_COUNT  6u

/* ── Fibo level constants ────────────────────── */
#define FRUSTUM_MAX_LEVELS 4u   /* L0..L3 */
#define FRUSTUM_L0         0u
#define FRUSTUM_L1         1u
#define FRUSTUM_L2         2u
#define FRUSTUM_L3         3u

/* ── State flags ─────────────────────────────── */
#define FRUSTUM_DORMANT    0u
#define FRUSTUM_MOUNTED    1u
#define FRUSTUM_GPU_READY  2u

/* ── FrustumLevel: one Fibo ring ─────────────── */
typedef struct {
    uint64_t  core;         /* derived core at this level                */
    uint32_t  addr;         /* fibo_addr(level, gear, world)             */
    uint8_t   gear;         /* G1/G2/G3 tier                             */
    uint8_t   world;        /* 0 = PHI_UP (expand), 1 = PHI_DOWN (shrink) */
    uint16_t  _pad;
} FrustumLevel;

/* ── FrustumNode: full frustum unit ─────────── */
typedef struct {
    uint64_t      wide_core;                    /* base (wide end) core  */
    uint64_t      apex_core;                    /* tip (narrow end) core */
    FrustumLevel  levels[FRUSTUM_MAX_LEVELS];   /* L0..L3 rings          */
    uint8_t       dir;                          /* FRUSTUM_DIR_*         */
    uint8_t       state;                        /* DORMANT/MOUNTED/GPU   */
    uint8_t       level_count;                  /* active levels used    */
    uint8_t       _pad;
} FrustumNode;

/* ── frustum_init: build FrustumNode from wide_core + direction ── */
static inline void frustum_init(FrustumNode *fn,
                                uint64_t wide_core,
                                uint8_t  dir)
{
    fn->wide_core   = wide_core;
    fn->dir         = dir;
    fn->state       = FRUSTUM_DORMANT;
    fn->level_count = FRUSTUM_MAX_LEVELS;

    uint64_t cur = wide_core;
    for (uint8_t i = 0; i < FRUSTUM_MAX_LEVELS; i++) {
        FrustumLevel *lv = &fn->levels[i];
        /* world alternates: even = expand (PHI_UP), odd = compress */
        lv->world = i & 1u;
        lv->gear  = (i < 4) ? i : 3u;   /* clamp to G3 */
        lv->addr  = fibo_addr(i, lv->gear, lv->world);
        lv->core  = derive_next_core(cur, dir, i);
        cur       = lv->core;
    }
    fn->apex_core = cur;
}

/* ── frustum_scale_addr: get fibo addr at given level ── */
static inline uint32_t frustum_scale_addr(const FrustumNode *fn,
                                          uint8_t level)
{
    if (level >= fn->level_count) return 0u;
    return fn->levels[level].addr;
}

/* ── frustum_slope_ratio: PHI_UP / PHI_DOWN ratio as fixed Q20 ── */
static inline uint32_t frustum_slope_ratio(uint8_t world)
{
    /* expand = PHI_UP, shrink = PHI_DOWN (both Q20 fixed point) */
    return world ? PHI_DOWN : PHI_UP;
}

#endif /* GEO_FRUSTUM_NODE_H */
