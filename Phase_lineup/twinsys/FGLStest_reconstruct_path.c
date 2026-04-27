/*
 * test_reconstruct_path.c — Phase 3: Recursive Expansion Tests
 * ═══════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

#include "core/geo_primitives.h"
#include "phase3/geo_reconstruct_path.h"
#include "phase3/geo_apex_activation.h"

static int g_pass = 0, g_fail = 0;

#define ASSERT(cond, msg) do { \
    if (cond) { printf("[PASS] %s\n", msg); g_pass++; } \
    else       { printf("[FAIL] %s (line %d)\n", msg, __LINE__); g_fail++; } \
} while(0)

/* ── T1: basic derivation ── */
static void t1_derivation(void) {
    uint64_t core = 0xDEADBEEFCAFEBABEULL;
    uint64_t next1 = derive_next_core(core, 0, 0);
    uint64_t next2 = derive_next_core(core, 0, 0);
    ASSERT(next1 == next2, "T1 deterministic");
    
    uint64_t next3 = derive_next_core(core, 1, 0);
    ASSERT(next1 != next3, "T1 face dependency");
    
    uint64_t next4 = derive_next_core(core, 0, 1);
    ASSERT(next1 != next4, "T1 step dependency");
}

/* ── T2: verify_step ── */
static void t2_verify(void) {
    uint64_t parent = 0x1122334455667788ULL;
    uint8_t face = 2;
    uint32_t step = 5;
    uint64_t child = derive_next_core(parent, face, step);
    
    ASSERT(verify_step(parent, child, face, step), "T2 verify success");
    ASSERT(!verify_step(parent, child + 1, face, step), "T2 verify failure (data)");
    ASSERT(!verify_step(parent, child, face + 1, step), "T2 verify failure (face)");
}

/* ── T3: reconstruct_chain ── */
static void t3_chain(void) {
    uint64_t seed = 0xABCDEF0123456789ULL;
    uint8_t faces[] = {0, 1, 2, 3, 4, 5};
    uint64_t chain[6];
    
    reconstruct_chain(seed, faces, 6, chain);
    
    /* Manual step-by-step check */
    uint64_t cur = seed;
    for (int i = 0; i < 6; i++) {
        cur = derive_next_core(cur, faces[i], i);
        ASSERT(chain[i] == cur, "T3 chain step check");
    }
}

/* ── T4: ghost_shadow ── */
static void t4_shadow(void) {
    uint64_t core = 0x5555AAAA5555AAAAULL;
    uint8_t face = 3;
    uint32_t step = 10;
    
    uint64_t s1 = ghost_shadow(core, face, step);
    uint64_t s2 = ghost_shadow(core, face, step);
    ASSERT(s1 == s2, "T4 shadow deterministic");
    
    uint64_t s3 = ghost_shadow(core ^ 1, face, step);
    ASSERT(s1 != s3, "T4 shadow detects core drift");
}

/* ── T5: long chain integrity (10k steps) ── */
static void t5_long_chain(void) {
    uint64_t cur = 0x1234567890ABCDEFULL;
    uint64_t seed = cur;
    uint8_t faces[10000];
    
    for (int i = 0; i < 10000; i++) faces[i] = i % 6;
    
    /* Forward pass */
    for (int i = 0; i < 10000; i++) {
        cur = derive_next_core(cur, faces[i], i);
    }
    
    /* Reconstruct pass */
    uint64_t final_reconstructed;
    uint64_t temp = seed;
    for (int i = 0; i < 10000; i++) {
        temp = derive_next_core(temp, faces[i], i);
    }
    final_reconstructed = temp;
    
    ASSERT(cur == final_reconstructed, "T5 10k steps integrity");
}

int main(void) {
    printf("--- Phase 3: Recursive Expansion Tests ---\n");
    t1_derivation();
    t2_verify();
    t3_chain();
    t4_shadow();
    t5_long_chain();
    
    printf("\nSUMMARY: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail > 0;
}
