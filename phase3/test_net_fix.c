/*
 * test_net_fix.c — N3 & N4 Fix Tests
 * ═══════════════════════════════════════════════════════════════════
 *
 * N3: dodeca_score fix
 * N4: Barrett constant fix
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include "phase3/geo_net_fix.h"

/* ── Test helpers ── */
static int g_pass = 0, g_fail = 0;

#define ASSERT(cond, msg) do { \
    if (cond) { printf("[PASS] %s\n", msg); g_pass++; } \
    else       { printf("[FAIL] %s (line %d)\n", msg, __LINE__); g_fail++; } \
} while(0)

/* ═══════════════════════════════════════════════════════════════════
 * N4: Barrett constant tests
 * ═══════════════════════════════════════════════════════════════════ */

static void test_n4_mod6_accuracy(void) {
    /* Test both versions for correctness */
    for (uint32_t n = 0; n < 1000; n++) {
        uint8_t r_fixed = _gn_mod6_fixed(n);
        uint8_t r_orig = _gn_mod6_original(n);
        uint8_t expected = n % 6;
        
        ASSERT(r_fixed == expected, "N4 fixed mod6 correct");
        ASSERT(r_orig == expected,  "N4 original mod6 correct");
    }
}

static void test_n4_boundary_cases(void) {
    /* Edge cases */
    ASSERT(_gn_mod6_fixed(0) == 0, "N4 0 % 6 = 0");
    ASSERT(_gn_mod6_fixed(6) == 0, "N4 6 % 6 = 0");
    ASSERT(_gn_mod6_fixed(5) == 5, "N4 5 % 6 = 5");
    ASSERT(_gn_mod6_fixed(12) == 0, "N4 12 % 6 = 0");
    ASSERT(_gn_mod6_fixed(3456) == 0, "N4 3456 (CYL_FULL_N) % 6 = 0");
    ASSERT(_gn_mod6_fixed(575) == 5, "N4 575 (max slot) % 6 = 5");
}

static void test_n4_consistency(void) {
    /* Fixed and original should match for small values */
    for (uint32_t n = 0; n < 100; n++) {
        ASSERT(_gn_mod6_fixed(n) == _gn_mod6_original(n), 
               "N4 both methods consistent");
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * N3: dodeca_score tests
 * ═══════════════════════════════════════════════════════════════════ */

static void test_n3_zero_input(void) {
    uint32_t score = calc_dodeca_score(NULL, 0);
    ASSERT(score == 0, "N3 NULL/zero input = 0");
}

static void test_n3_single_path(void) {
    uint64_t cores[1] = { 0xFFFFFFFFFFFFFFFFULL };
    uint32_t score = calc_dodeca_score(cores, 1);
    /* popcount(64 bits) = 64 → score = 64/8 = 8 */
    ASSERT(score == 8, "N3 single max entropy = 8");
}

static void test_n3_low_entropy(void) {
    uint64_t cores[1] = { 0x0000000000000000ULL };
    uint32_t score = calc_dodeca_score(cores, 1);
    /* popcount(0) = 0 → score = 0 */
    ASSERT(score == 0, "N3 zero entropy = 0");
}

static void test_n3_mixed_entropy(void) {
    uint64_t cores[3] = { 
        0xFFFFFFFFFFFFFFFFULL,  /* 64 bits → 8 */
        0x0000000000000000ULL,  /* 0 bits → 0 */
        0xAAAAAAAAAAAAAAAAULL    /* 32 bits → 4 */
    };
    uint32_t score = calc_dodeca_score(cores, 3);
    ASSERT(score == 12, "N3 mixed entropy sum = 12");
}

static void test_n3_verdict_pass(void) {
    /* Score 100 should pass (96-128) */
    ASSERT(dodeca_verdict(96) == 1, "N3 verdict 96 = pass");
    ASSERT(dodeca_verdict(100) == 1, "N3 verdict 100 = pass");
    ASSERT(dodeca_verdict(128) == 1, "N3 verdict 128 = pass");
}

static void test_n3_verdict_fail(void) {
    ASSERT(dodeca_verdict(0) == 0, "N3 verdict 0 = fail");
    ASSERT(dodeca_verdict(64) == 0, "N3 verdict 64 = fail");
    ASSERT(dodeca_verdict(200) == 0, "N3 verdict 200 = fail");
}

static void test_n3_update(void) {
    DodecaStats ds;
    dodeca_reset(&ds);
    
    uint64_t cores[2] = { 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL };
    dodeca_update(&ds, cores, 2);
    
    ASSERT(ds.dodeca_score == 16, "N3 update score = 16");
    ASSERT(ds.valid_paths == 2,   "N3 update paths = 2");
    ASSERT(ds.last_verdict == 0,  "N3 update verdict = fail");
}

static void test_n3_full_paths(void) {
    uint64_t cores[12];
    for (int i = 0; i < 12; i++) cores[i] = 0xFFFFFFFFFFFFFFFFULL;
    
    DodecaStats ds;
    dodeca_update(&ds, cores, 12);
    
    ASSERT(ds.valid_paths == 12, "N3 full paths = 12");
    ASSERT(ds.dodeca_score == 96, "N3 full score = 96");
    ASSERT(ds.last_verdict == 1, "N3 full verdict = pass");
}

static void test_n3_overflow_paths(void) {
    /* More than 12 paths - should only count first 12 */
    uint64_t cores[15];
    for (int i = 0; i < 15; i++) cores[i] = 0xFFFFFFFFFFFFFFFFULL;
    
    DodecaStats ds;
    dodeca_update(&ds, cores, 15);
    
    ASSERT(ds.valid_paths == 12, "N3 overflow capped to 12");
    ASSERT(ds.dodeca_score == 96, "N3 overflow score = 96");
}

/* ═══════════════════════════════════════════════════════════════════
 * Main
 * ═══════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("=== N3 & N4 Fix Tests ===\n");
    printf("\n--- N4: Barrett Constant ---\n");
    test_n4_mod6_accuracy();
    test_n4_boundary_cases();
    test_n4_consistency();
    
    printf("\n--- N3: Dodeca Score ---\n");
    test_n3_zero_input();
    test_n3_single_path();
    test_n3_low_entropy();
    test_n3_mixed_entropy();
    test_n3_verdict_pass();
    test_n3_verdict_fail();
    test_n3_update();
    test_n3_full_paths();
    test_n3_overflow_paths();
    
    printf("\n===========================\n");
    printf("Result: %d/%d PASS\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}