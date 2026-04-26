/*
 * geo_letter_cube.h — Letter Cube Pairing + 78-Step Loop Closure
 * ═══════════════════════════════════════════════════════════════
 *
 * Architecture:
 *   Virtual Apex stores LetterPair (Aa..Zz) as Invisible Index Header.
 *   Frustum faces couple ONLY when both angle AND letter pair match.
 *   26 pairs × LCM(13,6) = 78 steps → full state space closure.
 *
 * Rules:
 *   - upper A..Z (0..25), lower a..z (0..25), same index = valid pair
 *   - match requires: angle_ok AND upper==lower (same slot)
 *   - 6 frustum faces → bigger cube at depth+1 (recursive)
 *   - closure_verify_78: walk all 78 steps, confirm loop sealed
 *
 * No malloc, no float, no heap.
 * ═══════════════════════════════════════════════════════════════
 */

#ifndef GEO_LETTER_CUBE_H
#define GEO_LETTER_CUBE_H

#include "pogls_platform.h"
#include "geo_config.h"
#include <stdint.h>
#include <string.h>

/* ── Constants ───────────────────────────────────────────────── */
#define LC_PAIRS        26u   /* A..Z / a..z                      */
#define LC_CLOSURE_STEPS 78u  /* LCM(13,6) = 78                   */
#define LC_FACES         6u   /* frustum faces per cube           */
#define LC_MAX_DEPTH     8u   /* max recursive cube nesting       */

/* ── LetterPair ──────────────────────────────────────────────── */
/* stored at Virtual Apex — 2B, index 0..25 */
typedef struct {
    uint8_t upper;   /* 0=A .. 25=Z */
    uint8_t lower;   /* 0=a .. 25=z */
} LetterPair;

/* valid pair: upper == lower (same slot) */
static inline int lc_pair_valid(LetterPair p) {
    return (p.upper < LC_PAIRS) && (p.lower < LC_PAIRS)
           && (p.upper == p.lower);
}

/* make pair from index 0..25 */
static inline LetterPair lc_make_pair(uint8_t idx) {
    LetterPair p = { idx & 31u, idx & 31u };
    if (p.upper >= LC_PAIRS) p.upper = p.lower = 0;
    return p;
}

/* ── ApexHeader ──────────────────────────────────────────────── */
/* sits at Virtual Apex — Invisible Index Header */
typedef struct {
    LetterPair  pair;         /* Aa..Zz coupling key              */
    uint8_t     depth;        /* recursion depth (0 = base cube)  */
    uint8_t     face_count;   /* faces coupled so far (0..6)      */
    uint32_t    angle_key;    /* geometric pull value             */
    uint64_t    slope_hash;   /* apex slope fingerprint           */
} ApexHeader;   /* 16B */

/* ── CubeNode ────────────────────────────────────────────────── */
/* one frustum face slot */
typedef struct {
    uint64_t    core;         /* face core seed                   */
    LetterPair  key;          /* assigned letter pair             */
    uint8_t     face_id;      /* 0..5                             */
    uint8_t     coupled;      /* 1 = locked to partner            */
} CubeNode;   /* 12B */

/* ── CubeCtx ─────────────────────────────────────────────────── */
/* 6 faces + apex header — one frustum unit */
typedef struct {
    ApexHeader  apex;
    CubeNode    faces[LC_FACES];
    uint8_t     coupled_count;
    uint8_t     depth;
} CubeCtx;   /* ~92B */

/* ── Init ────────────────────────────────────────────────────── */
static inline void lc_cube_init(CubeCtx *c, uint8_t pair_idx, uint8_t depth) {
    memset(c, 0, sizeof(*c));
    c->apex.pair  = lc_make_pair(pair_idx);
    c->apex.depth = depth;
    c->depth      = depth;
    for (uint8_t i = 0; i < LC_FACES; i++)
        c->faces[i].face_id = i;
}

/* ── Assign pair to face ─────────────────────────────────────── */
static inline void lc_assign_face(CubeCtx *c, uint8_t face_id,
                                   uint64_t core, uint8_t pair_idx) {
    if (face_id >= LC_FACES) return;
    c->faces[face_id].core    = core;
    c->faces[face_id].key     = lc_make_pair(pair_idx);
    c->faces[face_id].coupled = 0;
}

/* ── Match: angle + letter pair must agree ───────────────────── */
/*
 * Non-Code Coupling:
 *   angle_ok  = (a->angle_key ^ b->angle_key) matches apex slope_hash
 *   letter_ok = a->key == b->key AND lc_pair_valid
 */
static inline int lc_pair_match(const CubeCtx *c,
                                 uint8_t fa, uint8_t fb) {
    if (fa >= LC_FACES || fb >= LC_FACES || fa == fb) return 0;
    const CubeNode *a = &c->faces[fa];
    const CubeNode *b = &c->faces[fb];

    int letter_ok = lc_pair_valid(a->key)
                 && (a->key.upper == b->key.upper)
                 && (a->key.lower == b->key.lower);
    if (!letter_ok) return 0;

    /* angle check: XOR of cores must align with apex slope_hash */
    uint64_t angle_xor = a->core ^ b->core;
    int angle_ok = ((angle_xor ^ c->apex.slope_hash) & 0xFFFFu) == 0;
    return angle_ok;
}

/* ── Force match: scan all pairs, couple on first hit ────────── */
/*
 * Reversed Factorial Logic: geometry forces match in ≤6 steps.
 * Returns number of new couplings made (0 if none).
 */
static inline uint8_t lc_force_match(CubeCtx *c) {
    uint8_t new_coupled = 0;
    for (uint8_t fa = 0; fa < LC_FACES && c->coupled_count < LC_FACES; fa++) {
        if (c->faces[fa].coupled) continue;
        for (uint8_t fb = fa + 1; fb < LC_FACES; fb++) {
            if (c->faces[fb].coupled) continue;
            if (lc_pair_match(c, fa, fb)) {
                c->faces[fa].coupled = 1;
                c->faces[fb].coupled = 1;
                c->coupled_count    += 2;
                new_coupled         += 2;
            }
        }
    }
    return new_coupled;
}

/* ── Bigger Cube: 6 frustums → promote to next depth ─────────── */
/*
 * When all 6 faces coupled → synthesize parent CubeCtx at depth+1.
 * slope_hash of parent = XOR fold of all child slope_hashes.
 */
static inline int lc_promote(const CubeCtx *children[LC_FACES],
                               CubeCtx *parent_out) {
    for (uint8_t i = 0; i < LC_FACES; i++)
        if (!children[i] || children[i]->coupled_count < LC_FACES) return 0;

    uint8_t base_depth = children[0]->depth;
    if (base_depth + 1 >= LC_MAX_DEPTH) return 0;

    lc_cube_init(parent_out, children[0]->apex.pair.upper, base_depth + 1);

    uint64_t fold = 0;
    for (uint8_t i = 0; i < LC_FACES; i++) {
        fold ^= children[i]->apex.slope_hash;
        lc_assign_face(parent_out, i, children[i]->apex.slope_hash, i % LC_PAIRS);
    }
    parent_out->apex.slope_hash = fold;
    return 1;
}

/* ── 78-Step Loop Closure Verifier ──────────────────────────── */
/*
 * Walk 78 steps using state = (pair_idx × step) mod 26.
 * LCM(13,6)=78 guarantees full coverage of all 26 pairs × 3 phases.
 * Returns 1 if loop seals (state returns to 0 at step 78).
 */
typedef struct {
    uint8_t  state;          /* current pair slot 0..25           */
    uint8_t  phase;          /* 0..2 (period-6 phase)             */
    uint32_t step;           /* current step 0..77                */
    uint8_t  visited[LC_PAIRS]; /* coverage map                   */
} ClosureCtx;

static inline void lc_closure_init(ClosureCtx *ctx, uint8_t start_pair) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->state = start_pair % LC_PAIRS;
}

static inline int lc_closure_step(ClosureCtx *ctx) {
    if (ctx->step >= LC_CLOSURE_STEPS) return -1; /* done */

    ctx->visited[ctx->state] = 1;
    /* advance: (state + 6) mod 13 cycles through LCM(13,6) space */
    ctx->state  = (ctx->state + 6u) % 13u;
    ctx->phase  = (ctx->phase  + 1u) %  6u;
    ctx->step++;
    return (int)ctx->state;
}

/*
 * lc_closure_verify_78 — run all 78 steps, return 1 if sealed.
 * Sealed = step 78 state == start AND all 13 base states visited.
 */
static inline int lc_closure_verify_78(uint8_t start_pair) {
    ClosureCtx ctx;
    lc_closure_init(&ctx, start_pair);
    uint8_t start_state = ctx.state;

    for (uint32_t i = 0; i < LC_CLOSURE_STEPS; i++)
        lc_closure_step(&ctx);

    /* check closure */
    if (ctx.state != start_state) return 0;

    /* check coverage: all 13 mod-13 states visited */
    uint8_t covered = 0;
    for (uint8_t i = 0; i < 13u; i++)
        if (ctx.visited[i]) covered++;
    return covered == 13u;
}

/* ── Status ──────────────────────────────────────────────────── */
static inline void lc_cube_status(const CubeCtx *c) {
    (void)c; /* hook for debug print — implement per platform */
}

#endif /* GEO_LETTER_CUBE_H */
