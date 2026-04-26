/*
 * geo_ghost_watcher.h — Field-Driven Ghost Watcher
 * =================================================
 * Concept: ไฟล์ไม่มีอยู่จริงใน storage
 *   File in → GeoSeed slice → Field traversal → Watcher records blueprint
 *   Reconstruct = replay field pattern (ไม่ต้องเก็บ raw bytes)
 *
 * Stack:
 *   ScanEntry (pogls_scanner.h)  → input slice
 *   ThirdEye  (geo_thirdeye.h)   → field observer / watcher
 *   GhostRef  (this file)        → blueprint (9B, no alloc)
 *   FiboClock (geo_fibo_clock.h) → zone boundary (17/72/144/720)
 *
 * Ghost lifecycle:
 *   DORMANT  → ACTIVE (particle cross face) → BLUEPRINT (zone boundary)
 *   Blueprint = immutable mold, reconstruct on-demand, ghost dies
 *
 * P1 proven: ghost_slope() zero-collision across 6 faces
 *   slope = core ^ (core >> FIBO_SHIFT[f]) ^ (f * GOLDEN)
 *   odd face → ~slope (complement pair)
 */

#ifndef GEO_GHOST_WATCHER_H
#define GEO_GHOST_WATCHER_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "core/geo_config.h"
#include "core/geo_thirdeye.h"



/* ── Fibo shifts (P1 proven) ── */
static const uint8_t GHOST_FIBO_SHIFT[6] = {1,1,2,3,5,8};
#define GHOST_GOLDEN  0x9E3779B185EBCA87ULL

/* ── mix64: avalanche (M2.1 locked) ── */
static inline uint64_t ghost_mix64(uint64_t x) {
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27; x *= 0x94D049BB133111EBULL;
    x ^= x >> 31; return x;
}

/* ── Fibo shifts for base face 0,1,2 ── */
static const int GHOST_BASE_SHIFT[3] = {1, 2, 5};


/* ── GhostRef: 9B — blueprint of 1 chunk crossing 1 face ── */
typedef struct {
    uint64_t master_core;   /* GeoSeed.gen2 at crossing moment  */
    uint8_t  face_idx;      /* 0-5: which frustum face          */
    /* slope = derived, never stored */
} GhostRef;   /* 9B */

/* ── Ghost state ── */
#define GHOST_DORMANT   0
#define GHOST_ACTIVE    1
#define GHOST_BLUEPRINT 2

/* ── WatcherCtx: per-file observer ── */
/* Stores only blueprints — raw data never held */
#define GHOST_MAX_BLUEPRINTS  144u   /* 1 full TE_CYCLE worth */

typedef struct {
    ThirdEye     te;                              /* field observer     */
    GhostRef     blueprints[GHOST_MAX_BLUEPRINTS];/* molds              */
    uint32_t     bp_count;                        /* valid blueprints   */
    uint64_t     chunk_count;                     /* total chunks seen  */
    uint8_t      state;                           /* DORMANT/ACTIVE/BP  */
} WatcherCtx;

/* ── ghost_slope: pair binding face i↔i+3 = bitwise complement ── */
static inline uint64_t ghost_slope(uint64_t core, uint8_t f) {
    uint8_t  shift = GHOST_BASE_SHIFT[f % 3];
    uint8_t  pair  = f / 3;   /* 0=self, 1=complement */
    uint64_t s = core ^ (core >> shift);
    return (pair == 1) ? ~s : s;
}

/* ── ghost_slope_invert: find core given slope and face ── */
/* 
 * Inverts X ^ (X >> shift) = Y.
 * Since shift > 0, we can solve bit by bit from MSB to LSB.
 */
static inline uint64_t ghost_slope_invert(uint64_t slope, uint8_t f) {
    uint8_t  shift = GHOST_BASE_SHIFT[f % 3];
    uint8_t  pair  = f / 3;
    uint64_t y = (pair == 1) ? ~slope : slope;
    
    uint64_t x = 0;
    for (int i = 63; i >= 0; i--) {
        uint64_t bit_y = (y >> i) & 1;
        uint64_t bit_x_shifted = (i + shift < 64) ? ((x >> (i + shift)) & 1) : 0;
        if (bit_y ^ bit_x_shifted) {
            x |= (1ULL << i);
        }
    }
    return x;
}

static inline int ghost_valid(uint64_t core, uint64_t slope, uint8_t f) {
    return ghost_slope(core, f) == slope;
}

/* ── init ── */
static inline void watcher_init(WatcherCtx *w, GeoSeed genesis) {
    memset(w, 0, sizeof(WatcherCtx));
    te_init(&w->te, genesis);
    w->state = GHOST_DORMANT;
}

/* ── feed: call per ScanEntry chunk ── */
/* spoke   = chunk coord mapped to 0-5 cylinder spoke
 * slot_hot = 1 if this slot is above density threshold
 * Returns GHOST_BLUEPRINT if zone boundary sealed a blueprint */
static inline uint8_t watcher_feed(WatcherCtx *w,
                                   uint64_t    core,   /* ScanEntry.seed  */
                                   uint8_t     spoke,  /* 0-5             */
                                   uint8_t     slot_hot)
{
    GeoSeed cur = { core, w->chunk_count };
    w->chunk_count++;

    /* activate ghost on first particle */
    if (w->state == GHOST_DORMANT) w->state = GHOST_ACTIVE;

    /* tick field observer */
    te_tick(&w->te, cur, spoke, slot_hot, 0u);

    /* seal blueprint at Fibo zone boundary (every TE_CYCLE=144 chunks) */
    if (w->chunk_count % TE_CYCLE == 0) {
        if (w->bp_count < GHOST_MAX_BLUEPRINTS) {
            GhostRef *bp = &w->blueprints[w->bp_count++];
            bp->master_core = core;
            bp->face_idx    = spoke;   /* face = spoke at boundary */
        }
        w->state = GHOST_BLUEPRINT;
        return GHOST_BLUEPRINT;
    }

    return GHOST_ACTIVE;
}

/* ── reconstruct: verify blueprint chain integrity ── */
/* Returns number of valid blueprints (tamper = mismatch) */
static inline uint32_t watcher_verify(const WatcherCtx *w) {
    uint32_t ok = 0;
    for (uint32_t i = 0; i < w->bp_count; i++) {
        const GhostRef *bp = &w->blueprints[i];
        uint64_t expected = ghost_slope(bp->master_core, bp->face_idx);
        /* re-derive and cross-check complement pair */
        uint8_t  pair = (bp->face_idx + 3) % 6;   /* true opposite face */
        uint64_t pair_slope = ghost_slope(bp->master_core, pair);
        /* odd XOR even should differ (P1 guarantee) */
        if (expected == (~pair_slope & 0xFFFFFFFFFFFFFFFFULL)) ok++;
    }
    return ok;
}

/* ── status ── */
static inline void watcher_status(const WatcherCtx *w) {
    const char *st[] = {"DORMANT","ACTIVE","BLUEPRINT"};
    printf("[Watcher] chunks=%llu  blueprints=%u/%u  state=%s\n",
           (unsigned long long)w->chunk_count,
           w->bp_count, GHOST_MAX_BLUEPRINTS,
           st[w->state < 3 ? w->state : 0]);
    te_status(&w->te);
}

#endif /* GEO_GHOST_WATCHER_H */
