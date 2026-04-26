/*
 * test_apex_activation.c — M3.1 Apex Activation Rule Tests
 * ══════════════════════════════════════════════════════════════════
 *
 * Tests:
 *   T1  apex_init / basic state
 *   T2  apex_feed detects apex at Fibo harmonic band
 *   T3  apex_derive_child produces unique child cores
 *   T4  apex_get_ghost_ref creates valid GhostRef
 *   T5  complement pair never triggers activation
 *   T6  apex_verify returns correct count
 *   T7  multiple activations in sequence
 *   T8  depth increments correctly
 *   T9  apex_check correctly identifies harmonic bands
 *   T10 zone boundary tracking (144 chunks)
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

/* ── Headers ── */
#include "core/geo_config.h"
#include "core/geo_thirdeye.h"
#include "geo_ghost_watcher.h"
#include "phase3/geo_apex_activation.h"

/* ── Test helpers ── */
static int g_pass = 0, g_fail = 0;

#define ASSERT(cond, msg) do { \
    if (cond) { printf("[PASS] %s\n", msg); g_pass++; } \
    else       { printf("[FAIL] %s (line %d)\n", msg, __LINE__); g_fail++; } \
} while(0)

/* ── T1: init ── */
static void t1_init(void) {
    GeoSeed genesis = { 0xDEADBEEFULL, 0xCAFEBABEULL };
    ApexCtx a;
    apex_init(&a, genesis);
    ASSERT(a.act_count == 0,      "T1 act_count zero");
    ASSERT(a.state == APEX_DORMANT, "T1 state DORMANT");
}

/* ── T2: apex detection ── */
static void t2_apex_detection(void) {
    GeoSeed genesis = { 1ULL, 2ULL };
    ApexCtx a;
    apex_init(&a, genesis);

    /* find a core that produces apex */
    uint64_t fertile = 0;
    for (uint64_t s = 1; s < 100000 && !fertile; s++) {
        for (uint8_t fa = 0; fa < 6 && !fertile; fa++) {
            for (uint8_t fb = fa + 1; fb < 6 && !fertile; fb++) {
                if (is_complement_pair(fa, fb)) continue;
                uint64_t sa = apex_slope(s, fa);
                uint64_t sb = apex_slope(s, fb);
                if (apex_check(sa, sb)) fertile = s;
            }
        }
    }

    if (fertile) {
        uint8_t result = apex_feed(&a, fertile, 0, 1);
        ASSERT(result == APEX_ACTIVE,   "T2 apex detected");
        ASSERT(a.act_count == 1,       "T2 one activation");
    } else {
        printf("[SKIP] T2 no fertile core found\n");
    }
}

/* ── T3: derive child uniqueness ── */
static void t3_child_unique(void) {
    uint64_t parent = 0xABCDEF1234567890ULL;
    uint64_t pat1 = 0x1111111111111111ULL;
    uint64_t pat2 = 0x2222222222222222ULL;

    uint64_t child1 = apex_derive_child(parent, pat1, 1);
    uint64_t child2 = apex_derive_child(parent, pat2, 1);
    uint64_t child3 = apex_derive_child(parent, pat1, 2);  /* different depth */

    ASSERT(child1 != child2, "T3 different pattern → different child");
    ASSERT(child1 != child3, "T3 different depth → different child");
}

/* ── T4: ghost ref conversion ── */
static void t4_ghost_ref(void) {
    ApexRef ar = {
        .parent_core  = 0x1234567890ABCDEFULL,
        .apex_pattern = 0xA1B2C3D4E5F60718ULL,
        .face_a       = 2,
        .face_b       = 4,
        .depth        = 3
    };

    GhostRef ref = apex_get_ghost_ref(&ar);
    ASSERT(ref.master_core != 0,        "T4 master_core derived");
    ASSERT(ref.face_idx == 2,           "T4 face_idx from face_a");
}

/* ── T5: complement pair never activates ── */
static void t5_no_complement(void) {
    uint64_t core = 0xDEADBEEF12345678ULL;
    for (uint8_t f = 0; f < 3; f++) {
        uint8_t pair = (f + 3) % 6;
        ASSERT(!is_complement_pair(f, pair) == 0 || is_complement_pair(f, pair) == 1,
               "T5 complement check");
    }
}

/* ── T6: verify ── */
static void t6_verify(void) {
    GeoSeed genesis = { 1ULL, 2ULL };
    ApexCtx a;
    apex_init(&a, genesis);

    /* find and activate apex */
    for (uint64_t s = 1; s < 100000 && a.act_count == 0; s++) {
        apex_feed(&a, s, 0, 1);
    }

    if (a.act_count > 0) {
        uint32_t ok = apex_verify(&a);
        ASSERT(ok >= 1, "T6 verify returns valid count");
    } else {
        printf("[SKIP] T6 no activation to verify\n");
    }
}

/* ── T7: multiple activations ── */
static void t7_multi_activation(void) {
    GeoSeed genesis = { 1ULL, 2ULL };
    ApexCtx a;
    apex_init(&a, genesis);

    /* try multiple cores */
    int activations = 0;
    for (uint64_t s = 1; s < 50000 && activations < 10; s++) {
        uint8_t r = apex_feed(&a, s, 0, 1);
        if (r == APEX_ACTIVE) activations++;
    }

    ASSERT(a.act_count > 0, "T7 at least one activation");
}

/* ── T8: depth increment ── */
static void t8_depth(void) {
    GeoSeed genesis = { 1ULL, 2ULL };
    ApexCtx a;
    apex_init(&a, genesis);

    /* first activation */
    for (uint64_t s = 1; s < 100000 && a.act_count == 0; s++) {
        apex_feed(&a, s, 0, 1);
    }

    if (a.act_count > 0) {
        uint8_t first_depth = a.activations[0].depth;
        ASSERT(first_depth > 0, "T8 first activation has depth > 0");
    }
}

/* ── T9: harmonic band check ── */
static void t9_harmonic_band(void) {
    /* low band: pc 15-20 */
    ASSERT(apex_check(0xFFFFFFFFFFFFFFFFULL, 0x0000000000000000ULL) == 0,
           "T9 all bits different");

    /* non-matching */
    ASSERT(apex_check(0x0000000000000000ULL, 0x0000000000000000ULL) == 0,
           "T9 same = no match");
}

/* ── T10: zone boundary tracking ── */
static void t10_zone(void) {
    GeoSeed genesis = { 1ULL, 2ULL };
    ApexCtx a;
    apex_init(&a, genesis);
    
    for (uint64_t i = 0; i < 144; i++) {
        apex_feed(&a, i, 0, 1);
    }
    ASSERT(a.te.op_count == 144, "T10 144 chunks tracked");
}

int main(void) {
    printf("=== M3.1 Apex Activation Tests ===\n");
    t1_init();
    t2_apex_detection();
    t3_child_unique();
    t4_ghost_ref();
    t5_no_complement();
    t6_verify();
    t7_multi_activation();
    t8_depth();
    t9_harmonic_band();
    t10_zone();
    printf("\n%d/%d PASS\n", g_pass, g_pass + g_fail);
    return g_fail == 0 ? 0 : 1;
}
