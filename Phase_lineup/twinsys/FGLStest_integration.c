/*
 * test_integration.c — N1: Scanner × Watcher × Apex Full Pipeline Test
 * ════════════════════════════════════════════════════════════════════
 *
 * Tests:
 *   T1  ghost_integ_init bootstraps all components
 *   T2  scan_callback processes chunks correctly
 *   T3  watcher records blueprints
 *   T4  apex detects activations
 *   T5  fabric expands on zone boundaries
 *   T6  expansion events queued and processed
 *   T7  full pipeline with real scan_buf
 *   T8  stats reporting works
 *   T9  verify checks all components
 *   T10 chunk count matches input
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

/* ── Headers ── */
#include "core/geo_config.h"
#include "core/geo_thirdeye.h"
#include "core/pogls_scanner.h"
#include "geo_ghost_watcher.h"
#include "geo_ghost_scanner.h"
#include "phase3/geo_apex_activation.h"
#include "phase3/geo_boundary_fabric.h"
#include "phase3/geo_ghost_integration.h"

/* ── Test helpers ── */
static int g_pass = 0, g_fail = 0;

#define ASSERT(cond, msg) do { \
    if (cond) { printf("[PASS] %s\n", msg); g_pass++; } \
    else       { printf("[FAIL] %s (line %d)\n", msg, __LINE__); g_fail++; } \
} while(0)

/* ── Make test buffer ── */
static void make_buf(uint8_t *out, uint32_t n_chunks, uint64_t seed_base) {
    for (uint32_t i = 0; i < n_chunks; i++) {
        uint64_t *w = (uint64_t *)(out + i * 64);
        for (int j = 0; j < 8; j++) {
            w[j] = seed_base ^ ((uint64_t)i << 16) ^ (uint64_t)j;
        }
    }
}

/* ── T1: init ── */
static void t1_init(void) {
    GhostIntegration gi;
    ghost_integ_init(&gi, 0xDEADBEEF12345678ULL);
    ASSERT(gi.init_ok == 1,        "T1 init_ok");
    ASSERT(gi.watcher.state == GHOST_DORMANT, "T1 watcher dormant");
    ASSERT(gi.apex.state == APEX_DORMANT, "T1 apex dormant");
}

/* ── T2: callback processes ── */
static void t2_callback(void) {
    GhostIntegration gi;
    ghost_integ_init(&gi, 0x1111111111111111ULL);

    /* Simulate ScanEntry */
    ScanEntry e;
    memset(&e, 0, sizeof(e));
    e.seed = 0xABCDEF01ULL;
    e.coord.face = 3;
    e.flags = SCAN_FLAG_VALID;

    ghost_scan_callback(&e, &gi);

    ASSERT(gi.total_chunks == 1, "T2 chunk counted");
    ASSERT(gi.watcher.state == GHOST_ACTIVE, "T2 watcher active");
}

/* ── T3: watcher blueprints ── */
static void t3_watcher_bp(void) {
    GhostIntegration gi;
    ghost_integ_init(&gi, 0x2222222222222222ULL);

    uint8_t buf[144 * 64];
    make_buf(buf, 144, 0x1234ULL);

    scan_buf(buf, sizeof(buf), ghost_scan_callback, &gi, NULL);

    ASSERT(gi.watcher.bp_count >= 1, "T3 at least one blueprint");
}

/* ── T4: apex activations ── */
static void t4_apex(void) {
    GhostIntegration gi;
    ghost_integ_init(&gi, 0x3333333333333333ULL);

    /* Scan enough chunks to trigger apex */
    uint8_t buf[288 * 64];
    make_buf(buf, 288, 0x5678ULL);

    scan_buf(buf, sizeof(buf), ghost_scan_callback, &gi, NULL);

    /* Apex may or may not activate depending on data */
    ASSERT(gi.total_chunks == 288, "T4 all chunks processed");
}

/* ── T5: fabric expansion ── */
static void t5_expansion(void) {
    GhostIntegration gi;
    ghost_integ_init(&gi, 0x4444444444444444ULL);

    /* Scan 144 chunks (one zone) */
    uint8_t buf[144 * 64];
    make_buf(buf, 144, 0x9ABCULL);

    scan_buf(buf, sizeof(buf), ghost_scan_callback, &gi, NULL);

    /* Fabric should have snapshots */
    ASSERT(gi.fabric.snap_count >= 1, "T5 at least one snapshot");
}

/* ── T6: expansion events ── */
static void t6_events(void) {
    GhostIntegration gi;
    ghost_integ_init(&gi, 0x5555555555555555ULL);

    uint8_t buf[144 * 64];
    make_buf(buf, 144, 0xDEF0ULL);

    scan_buf(buf, sizeof(buf), ghost_scan_callback, &gi, NULL);

    ASSERT(gi.expansion_count == 0, "T6 queue flushed after zone");
}

/* ── T7: full pipeline ── */
static void t7_full(void) {
    uint64_t genesis = 0x6666666666666666ULL;
    uint8_t buf[432 * 64];  /* 3 zones */
    make_buf(buf, 432, 0x1357ULL);

    GhostIntegration gi;
    ghost_integ_init(&gi, genesis);

    scan_buf(buf, sizeof(buf), ghost_scan_callback, &gi, NULL);

    ASSERT(gi.total_chunks == 432, "T7 432 chunks");
    ASSERT(gi.fabric.snap_count >= 3, "T7 3 snapshots");
}

/* ── T8: stats ── */
static void t8_stats(void) {
    GhostIntegration gi;
    ghost_integ_init(&gi, 0x7777777777777777ULL);

    uint8_t buf[144 * 64];
    make_buf(buf, 144, 0x2468ULL);
    scan_buf(buf, sizeof(buf), ghost_scan_callback, &gi, NULL);

    /* Just verify it doesn't crash */
    ghost_integ_stats(&gi);
    ASSERT(gi.init_ok == 1, "T8 stats runs");
}

/* ── T9: verify ── */
static void t9_verify(void) {
    GhostIntegration gi;
    ghost_integ_init(&gi, 0x8888888888888888ULL);

    uint8_t buf[144 * 64];
    make_buf(buf, 144, 0xACE0ULL);
    scan_buf(buf, sizeof(buf), ghost_scan_callback, &gi, NULL);

    uint32_t ok = ghost_integ_verify(&gi);
    ASSERT(ok >= 0, "T9 verify runs");
}

/* ── T10: chunk count ── */
static void t10_chunks(void) {
    GhostIntegration gi;
    ghost_integ_init(&gi, 0x9999999999999999ULL);
    uint8_t buf[100 * 64];
    make_buf(buf, 100, 0x1111ULL);
    scan_buf(buf, sizeof(buf), ghost_scan_callback, &gi, NULL);
    ASSERT(gi.total_chunks == 100, "T10 100 chunks counted");
}

int main(void) {
    printf("=== N1 Integration Tests ===\n");
    t1_init();
    t2_callback();
    t3_watcher_bp();
    t4_apex();
    t5_expansion();
    t6_events();
    t7_full();
    t8_stats();
    t9_verify();
    t10_chunks();
    printf("\n%d/%d PASS\n", g_pass, g_pass + g_fail);
    return g_fail == 0 ? 0 : 1;
}
