/*
 * geo_big_cube.h — FGLS Big Cube Assembly (Phase 4)
 * ══════════════════════════════════════════════════
 *
 * Big Cube = CoreCube + 3 FrustumPairs (6 FrustumNodes total)
 *
 * Assembly layout:
 *
 *          [Y+]
 *           │
 *   [X-] ──[CORE]── [X+]     ← face view
 *           │
 *          [Y-]
 *   (+ Z+/Z- into/out of page)
 *
 * Frustum pair pull sequence (same order as pair activation):
 *   pair[0] = X axis → X+/X- frustums mount after CUBE_AXIS_X locks
 *   pair[1] = Y axis → Y+/Y- frustums mount after CUBE_AXIS_Y locks
 *   pair[2] = Z axis → Z+/Z- frustums mount after CUBE_AXIS_Z locks
 *
 * Tessellation:
 *   Big Cubes share frustum wide-ends → neighbor discovery via
 *   wide_core XOR → deterministic, no pointer chasing
 */

#ifndef GEO_BIG_CUBE_H
#define GEO_BIG_CUBE_H

#include <stdint.h>
#include "geo_core_cube.h"
#include "geo_frustum_node.h"

/* ── FrustumPair: one axis, two frustums ─────── */
typedef struct {
    FrustumNode pos;        /* +axis frustum                         */
    FrustumNode neg;        /* -axis frustum                         */
    uint8_t     axis;       /* CUBE_AXIS_X/Y/Z                       */
    uint8_t     mounted;    /* 0 = dormant, 1 = mounted              */
    uint16_t    _pad;
} FrustumPair;

/* ── BigCube ─────────────────────────────────── */
typedef struct {
    CoreCube    core;
    FrustumPair pairs[CUBE_AXIS_COUNT];
    uint8_t     pairs_mounted;          /* 0..3                      */
    uint8_t     gpu_ready;              /* all 3 mounted + CUBE_MOUNTED */
    uint16_t    _pad;
} BigCube;

/* ── big_cube_init ───────────────────────────── */
static inline void big_cube_init(BigCube *bc, uint64_t seed)
{
    core_cube_init(&bc->core, seed);
    bc->pairs_mounted = 0u;
    bc->gpu_ready     = 0u;

    for (uint8_t ax = 0; ax < CUBE_AXIS_COUNT; ax++) {
        FrustumPair *fp = &bc->pairs[ax];
        fp->axis    = ax;
        fp->mounted = 0u;

        /* frustum wide_core = lock_core of matching pair (set at mount time) */
        /* directions: axis*2 = pos, axis*2+1 = neg  (matches FRUSTUM_DIR_*) */
        uint64_t pos_wide = derive_next_core(seed, ax * 2u,      1u);
        uint64_t neg_wide = derive_next_core(seed, ax * 2u + 1u, 1u);
        frustum_init(&fp->pos, pos_wide, ax * 2u);
        frustum_init(&fp->neg, neg_wide, ax * 2u + 1u);
    }
}

/* ── big_cube_activate_pair: lock apex pair + pull frustums ── */
/* Returns 1 when all 3 pairs mounted (gpu_ready) */
static inline int big_cube_activate_pair(BigCube *bc, uint8_t axis)
{
    int cube_complete = core_cube_lock_pair(&bc->core, axis);

    if (bc->core.pairs[axis].state == APEX_ACTIVE) {
        FrustumPair *fp = &bc->pairs[axis];
        if (!fp->mounted) {
            /* re-anchor frustum wide_core to actual lock_core */
            uint64_t lk = bc->core.pairs[axis].lock_core;
            fp->pos.wide_core = derive_next_core(lk, axis * 2u,      0u);
            fp->neg.wide_core = derive_next_core(lk, axis * 2u + 1u, 0u);
            fp->pos.state = FRUSTUM_MOUNTED;
            fp->neg.state = FRUSTUM_MOUNTED;
            fp->mounted   = 1u;
            bc->pairs_mounted++;
        }
    }

    if (cube_complete && bc->pairs_mounted == CUBE_AXIS_COUNT) {
        core_cube_mount(&bc->core);
        bc->gpu_ready = 1u;
        return 1;
    }
    return 0;
}

/* ── big_cube_neighbor_core: get shared wide_core with neighbor ── */
/* Each face's wide_core is the handshake key with the adjacent BigCube */
static inline uint64_t big_cube_neighbor_core(const BigCube *bc,
                                               uint8_t dir)
{
    uint8_t ax  = dir >> 1u;        /* axis = dir / 2                */
    uint8_t neg = dir &  1u;        /* 0 = pos, 1 = neg              */
    if (ax >= CUBE_AXIS_COUNT) return 0ULL;
    const FrustumNode *fn = neg ? &bc->pairs[ax].neg
                                : &bc->pairs[ax].pos;
    return fn->apex_core;           /* apex_core = neighbor's seed   */
}

#endif /* GEO_BIG_CUBE_H */
