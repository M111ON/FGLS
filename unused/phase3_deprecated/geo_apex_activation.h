/*
 * geo_apex_activation.h — M3.1 Apex Activation Rule
 * ══════════════════════════════════════════════════════════════════
 */

#ifndef GEO_APEX_ACTIVATION_H
#define GEO_APEX_ACTIVATION_H

#include <stdint.h>
#include <string.h>
#include "core/geo_primitives.h"
#include "geo_ghost_watcher.h"
#include "core/geo_config.h"
#include "core/geo_thirdeye.h"

/* ── Fibo harmonic bands ── */
#define APEX_PC_LOW_MIN   15
#define APEX_PC_LOW_MAX   20
#define APEX_PC_HIGH_MIN  44
#define APEX_PC_HIGH_MAX  49

/* ── Apex state ── */
#define APEX_DORMANT   0
#define APEX_SEARCHING 1
#define APEX_ACTIVE    2

/* ── ApexRef: 10B — activation record ── */
typedef struct {
    uint64_t parent_core;    /* original master_core               */
    uint64_t apex_pattern;   /* slope_a ^ slope_b at activation    */
    uint8_t  face_a;        /* first face in pair                  */
    uint8_t  face_b;        /* second face in pair                 */
    uint8_t  step;          /* sequence index (0 = first child)    */
} ApexRef;   /* 10B */

#define APEX_MAX_ACTIVATIONS 144u

typedef struct {
    ThirdEye  te;
    ApexRef   activations[APEX_MAX_ACTIVATIONS];
    uint32_t  act_count;
    uint8_t   state;
    uint8_t   _pad[2];
} ApexCtx;

/* ── derive_next_core: forward declaration or include ── */
/* Since geo_reconstruct_path.h includes this, we can't include it back easily.
   We'll define derive_next_core in a shared header or here.
   Actually, derive_next_core is in geo_reconstruct_path.h.
   I'll move it to geo_primitives.h to avoid circularity. */

static inline uint64_t apex_derive_child(uint64_t core, uint8_t face, uint32_t step) {
    uint64_t slope = _mix64(core ^ (face + step));
    return derive_next_core(core ^ slope, face, step);
}

/* ── Helper: check if popcount is in Fibo harmonic band ── */
static inline int apex_check(uint64_t c0, uint64_t c1) {
    uint64_t xr = c0 ^ c1;
    int pc = __builtin_popcountll(xr);
    return (pc >= APEX_PC_LOW_MIN && pc <= APEX_PC_LOW_MAX) ||
           (pc >= APEX_PC_HIGH_MIN && pc <= APEX_PC_HIGH_MAX);
}

static inline int is_complement_pair(uint8_t a, uint8_t b) {
    return (a < 6 && b < 6 && ((a + 3) % 6 == b || (b + 3) % 6 == a));
}

static inline uint64_t apex_slope(uint64_t core, uint8_t f) {
    return ghost_slope(core, f);
}

static inline void apex_init(ApexCtx *a, GeoSeed genesis) {
    memset(a, 0, sizeof(ApexCtx));
    te_init(&a->te, genesis);
}

static inline uint8_t apex_feed(ApexCtx *a, uint64_t core, uint8_t face, uint8_t slot_hot, uint32_t step) {
    GeoSeed cur = { core, 0 };
    te_tick(&a->te, cur, face, slot_hot, 0u);

    for (uint8_t fa = 0; fa < 6; fa++) {
        for (uint8_t fb = fa + 1; fb < 6; fb++) {
            uint64_t sa = ghost_slope(core, fa);
            uint64_t sb = ghost_slope(core, fb);
            if (!apex_check(sa, sb)) continue;

            if (a->act_count < APEX_MAX_ACTIVATIONS) {
                ApexRef *ar = &a->activations[a->act_count++];
                ar->parent_core  = core;
                ar->apex_pattern = sa ^ sb;
                ar->face_a       = fa;
                ar->face_b       = fb;
                ar->step         = (uint8_t)step;
            }
            return 2; /* ACTIVE */
        }
    }
    return 1; /* SEARCHING */
}

static inline GhostRef apex_get_ghost_ref(const ApexRef *ar) {
    GhostRef ref;
    ref.master_core = apex_derive_child(ar->parent_core, ar->face_a, ar->step);
    ref.face_idx    = ar->face_a;
    return ref;
}

/* ── reconstruct: verify apex chain ── */
static inline uint32_t apex_verify(const ApexCtx *a) {
    uint32_t ok = 0;
    for (uint32_t i = 0; i < a->act_count; i++) {
        const ApexRef *ar = &a->activations[i];
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
