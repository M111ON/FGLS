/*
 * geo_apex_activation.h — M3.1 Apex Activation Rule
 * ══════════════════════════════════════════════════════════════════
 *
 * Concept: จุดที่ slope ทั้ง 4 edges บรรจบ = Apex
 *          ถ้า collision pattern ที่ apex ตรงกับ Fibo harmonic → activate
 *
 * Apex Activation Flow:
 *   1. Find apex: 4 edges converge → single point in frustum space
 *   2. Check: apex_collision_pattern matches Fibo harmonic band
 *   3. If match: activate apex → create GhostRef
 *
 * Activation produces:
 *   - GhostRef (new) — geometric child, NOT a copy
 *   - derived master_core = f(parent_core, apex_pattern)
 *
 * Design Invariants (LOCKED):
 *   1. Ghost ไม่มี heap allocation เด็ดขาด
 *   2. master_core → ghost derivation ต้องเป็น pure function (no side effects)
 *   3. complement pair: slope(face i) + slope(face i+3) = constant
 *   4. ทุก expansion step ต้องผ่าน Fibo zone gate
 *   5. Blueprint = event record เท่านั้น ไม่ใช่ content copy
 *
 * Fibo harmonic band (from pogls_ghost_m23.c):
 *   - pc >= 15 && pc <= 20  (low band)
 *   - pc >= 44 && pc <= 49  (high band)
 *   where pc = popcount(slope_a ^ slope_b)
 */

#ifndef GEO_APEX_ACTIVATION_H
#define GEO_APEX_ACTIVATION_H

#include <stdint.h>
#include <string.h>
#include "geo_ghost_watcher.h"   /* GhostRef, watcher_feed, GHOST_* */
#include "core/geo_config.h"     /* TE_CYCLE */
#include "core/geo_thirdeye.h"    /* ThirdEye, GeoSeed */

/* ── Fibo harmonic bands (LOCKED from M2.3) ── */
#define APEX_PC_LOW_MIN   15
#define APEX_PC_LOW_MAX   20
#define APEX_PC_HIGH_MIN  44
#define APEX_PC_HIGH_MAX  49

/* ── Apex state ── */
#define APEX_DORMANT   0
#define APEX_SEARCHING 1
#define APEX_ACTIVE   2

/* ── ApexRef: 10B — activation record ── */
typedef struct {
    uint64_t parent_core;    /* original master_core               */
    uint64_t apex_pattern;   /* slope_a ^ slope_b at activation    */
    uint8_t  face_a;        /* first face in pair                  */
    uint8_t  face_b;        /* second face in pair                 */
    uint8_t  depth;         /* generation depth (0 = first child)  */
    /* master_core = derived via apex_derive_child()               */
} ApexRef;   /* 10B */

/* ── ApexCtx: per-expansion apex tracker ── */
#define APEX_MAX_ACTIVATIONS 144u   /* TE_CYCLE = Fibo zone boundary */

typedef struct {
    ThirdEye  te;
    ApexRef   activations[APEX_MAX_ACTIVATIONS];
    uint32_t  act_count;
    uint8_t   state;
    uint8_t   _pad[2];
} ApexCtx;

/* ── Helper: check if popcount is in Fibo harmonic band ── */
static inline int apex_check(uint64_t c0, uint64_t c1) {
    uint64_t xr = c0 ^ c1;
    int pc = __builtin_popcountll(xr);
    return (pc >= APEX_PC_LOW_MIN && pc <= APEX_PC_LOW_MAX) ||
           (pc >= APEX_PC_HIGH_MIN && pc <= APEX_PC_HIGH_MAX);
}

/* ── Complement pair check ── */
static inline int is_complement_pair(uint8_t a, uint8_t b) {
    return (a < 6 && b < 6 && ((a + 3) % 6 == b || (b + 3) % 6 == a));
}

/* ── ghost_slope from geo_ghost_watcher.h (re-export) ── */
static inline uint64_t apex_slope(uint64_t core, uint8_t f) {
    return ghost_slope(core, f);
}

/* ── Derive child core from parent + apex pattern ── */
/*
 * master_core derivation (P1 locked):
 *   new_core = mix64(parent ^ apex_pat ^ (depth * GOLDEN))
 *
 * This is a PURE function — no side effects, deterministic.
 */
static inline uint64_t apex_derive_child(uint64_t parent_core,
                                        uint64_t apex_pat,
                                        uint8_t  depth)
{
    uint64_t d = (uint64_t)depth * GHOST_GOLDEN;
    return ghost_mix64(parent_core ^ apex_pat ^ d);
}

/* ── init ── */
static inline void apex_init(ApexCtx *a, GeoSeed genesis) {
    memset(a, 0, sizeof(ApexCtx));
    te_init(&a->te, genesis);
    a->state = APEX_DORMANT;
}

/* ── feed: scan for apex activation ── */
/*
 * Called per chunk during field traversal.
 * Checks all face pairs for Fibo harmonic match.
 * Returns APEX_ACTIVE if new activation recorded.
 */
static inline uint8_t apex_feed(ApexCtx *a,
                                uint64_t    core,
                                uint8_t     face,
                                uint8_t     slot_hot)
{
    GeoSeed cur = { core, 0 };
    te_tick(&a->te, cur, face, slot_hot, 0u);

    /* start searching on first chunk */
    if (a->state == APEX_DORMANT) a->state = APEX_SEARCHING;

    /* check face pairs for apex */
    for (uint8_t fa = 0; fa < 6; fa++) {
        for (uint8_t fb = fa + 1; fb < 6; fb++) {
            if (is_complement_pair(fa, fb)) continue;  /* skip entanglement */

            uint64_t sa = apex_slope(core, fa);
            uint64_t sb = apex_slope(core, fb);

            if (!apex_check(sa, sb)) continue;

            /* found apex! record activation */
            if (a->act_count < APEX_MAX_ACTIVATIONS) {
                ApexRef *ar = &a->activations[a->act_count++];
                ar->parent_core  = core;
                ar->apex_pattern = sa ^ sb;
                ar->face_a       = fa;
                ar->face_b       = fb;
                ar->depth        = (uint8_t)a->act_count;  /* generation */
            }
            a->state = APEX_ACTIVE;
            return APEX_ACTIVE;
        }
    }

    return a->state;
}

/* ── get_ghost_ref: derive GhostRef from ApexRef ── */
/*
 * Convert activation record to GhostRef for watcher.
 * This is the bridge: apex activation → ghost creation.
 */
static inline GhostRef apex_get_ghost_ref(const ApexRef *ar) {
    GhostRef ref;
    ref.master_core = apex_derive_child(ar->parent_core,
                                       ar->apex_pattern,
                                       ar->depth);
    ref.face_idx    = ar->face_a;   /* use first face as canonical */
    return ref;
}

/* ── reconstruct: verify apex chain ── */
static inline uint32_t apex_verify(const ApexCtx *a) {
    uint32_t ok = 0;
    for (uint32_t i = 0; i < a->act_count; i++) {
        const ApexRef *ar = &a->activations[i];
        /* verify slope complement pair */
        uint64_t sa = apex_slope(ar->parent_core, ar->face_a);
        uint64_t sb = apex_slope(ar->parent_core, ar->face_b);
        if (apex_check(sa, sb)) ok++;
    }
    return ok;
}

/* ── status ── */
static inline void apex_status(const ApexCtx *a) {
    const char *st[] = {"DORMANT", "SEARCHING", "ACTIVE"};
    printf("[Apex] activations=%u/%u  state=%s\n",
           a->act_count, APEX_MAX_ACTIVATIONS,
           st[a->state < 3 ? a->state : 0]);
    te_status(&a->te);
}

#endif /* GEO_APEX_ACTIVATION_H */
