/*
 * geo_core_cube.h — FGLS Core Cube (Phase 4)
 * ═══════════════════════════════════════════
 *
 * Core Cube = convergence point ของ apex 6 อัน
 * activate เป็น pair ทีละคู่ sequential: X → Y → Z
 *
 * Analogy: เหมือน lock tumbler กุญแจ 3 ชั้น
 *   ต้องหมุน X ก่อน → Y → Z จึง unlock
 *   ถ้า pair ใดไม่ผ่าน slope check → cube ไม่ mount
 *
 * Pair state machine:
 *   DORMANT → SEARCHING → LOCKED → (next pair)
 *   ครบ 3 pairs → CUBE_READY → mount level 0
 */

#ifndef GEO_CORE_CUBE_H
#define GEO_CORE_CUBE_H

#include <stdint.h>
#include "../phase3/geo_apex_activation.h"
#include "geo_frustum_node.h"
#include "../core/geo_config.h"

/* ── Axis pair indices ───────────────────────── */
#define CUBE_AXIS_X   0u
#define CUBE_AXIS_Y   1u
#define CUBE_AXIS_Z   2u
#define CUBE_AXIS_COUNT 3u

/* ── Cube state ──────────────────────────────── */
#define CUBE_DORMANT    0u
#define CUBE_PARTIAL    1u   /* 1-2 pairs locked                    */
#define CUBE_READY      2u   /* all 3 pairs locked → can mount      */
#define CUBE_MOUNTED    3u   /* level 0 active, awaiting GPU        */

/* ── ApexPair: one axis pair of 2 apexes ─────── */
typedef struct {
    ApexRef   pos;          /* +direction apex                       */
    ApexRef   neg;          /* -direction apex                       */
    uint64_t  lock_core;    /* derived center core when pair locks   */
    uint8_t   axis;         /* CUBE_AXIS_X/Y/Z                       */
    uint8_t   state;        /* APEX_DORMANT / APEX_SEARCHING / APEX_ACTIVE */
    uint16_t  _pad;
} ApexPair;

/* ── CoreCube: full cube waiting to mount ─────── */
typedef struct {
    ApexPair  pairs[CUBE_AXIS_COUNT];   /* X, Y, Z pairs             */
    uint64_t  master_core;              /* XOR-fold of 3 lock_cores  */
    uint8_t   pairs_locked;             /* 0..3 counter              */
    uint8_t   state;                    /* CUBE_* state              */
    uint16_t  _pad;
} CoreCube;

/* ── core_cube_init ──────────────────────────── */
static inline void core_cube_init(CoreCube *cc, uint64_t seed)
{
    cc->master_core  = seed;
    cc->pairs_locked = 0u;
    cc->state        = CUBE_DORMANT;

    for (uint8_t ax = 0; ax < CUBE_AXIS_COUNT; ax++) {
        ApexPair *p = &cc->pairs[ax];
        p->axis      = ax;
        p->state     = APEX_DORMANT;
        p->lock_core = 0ULL;

        /* derive pos/neg apex seeds from master + axis */
        uint64_t pos_seed = derive_next_core(seed, ax * 2u,     0u);
        uint64_t neg_seed = derive_next_core(seed, ax * 2u + 1u, 0u);
        p->pos = (ApexRef){ .parent_core = pos_seed };
        p->neg = (ApexRef){ .parent_core = neg_seed };
    }
}

/* ── core_cube_lock_pair: call when apex pair converges ── */
/* Returns 1 if this call completed the cube (all 3 locked) */
static inline int core_cube_lock_pair(CoreCube *cc, uint8_t axis)
{
    if (axis >= CUBE_AXIS_COUNT) return 0;

    /* must lock in order X → Y → Z */
    if (axis != cc->pairs_locked) return 0;

    ApexPair *p = &cc->pairs[axis];
    if (p->state == APEX_ACTIVE) return 0;   /* already locked */

    /* derive lock_core: fold pos + neg apex cores */
    p->lock_core = p->pos.parent_core ^ _rotl64(p->neg.parent_core, 32u);
    p->state     = APEX_ACTIVE;
    cc->pairs_locked++;

    /* fold into master_core */
    cc->master_core ^= _rotl64(p->lock_core, axis * 13u);

    if (cc->pairs_locked == CUBE_AXIS_COUNT) {
        cc->state = CUBE_READY;
        return 1;
    }
    cc->state = CUBE_PARTIAL;
    return 0;
}

/* ── core_cube_mount: transition READY → MOUNTED ── */
static inline int core_cube_mount(CoreCube *cc)
{
    if (cc->state != CUBE_READY) return 0;
    cc->state = CUBE_MOUNTED;
    return 1;
}

#endif /* GEO_CORE_CUBE_H */
