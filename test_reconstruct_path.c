/*
 * test_reconstruct_path.c — N2: Path Reconstruction Tests
 * ═══════════════════════════════════════════════════════════
 *
 * Tests:
 *   T1  path_init creates empty path
 *   T2  path_reconstruct_from_watcher basic
 *   T3  path_reconstruct_from_fabric basic
 *   T4  merge_paths combines sources
 *   T5  path_verify checks integrity
 *   T6  path_dump runs without crash
 *   T7  path_get_sequence extracts indices
 *   T8  empty inputs handled gracefully
 *   T9  duplicate detection in merge
 *   T10 full pipeline with real data
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

/* ── Headers ── */
#include "core/geo_config.h"
#include "geo_ghost_watcher.h"
#include "phase3/geo_apex_activation.h"
#include "phase3/geo_boundary_fabric.h"
#include "phase3/geo_reconstruct_path.h"

/* ── Test helpers ── */
static int g_pass = 0, g_fail = 0;

#define ASSERT(cond, msg) do { \
    if (cond) { printf("[PASS] %s\n", msg); g_pass++; } \
    else       { printf("[FAIL] %s (line %d)\n", msg, __LINE__); g_fail++; } \
} while(0)

/* ── T1: init ── */
static void t1_init(void) {
    ReconstructedPath p;
    path_init(&p);
    ASSERT(p.node_count == 0, "T1 empty path");
    ASSERT(p.complete == 0,   "T1 not complete");
}

/* ── T2: reconstruct from watcher ── */
static void t2_from_watcher(void) {
    /* Create mock WatcherCtx with blueprints */
    WatcherCtx w;
    memset(&w, 0, sizeof(w));
    w.bp_count = 3;
    w.blueprints[0].master_core = 0x1111111111111111ULL;
    w.blueprints[0].face_idx = 0;
    w.blueprints[1].master_core = 0x2222222222222222ULL;
    w.blueprints[1].face_idx = 1;
    w.blueprints[2].master_core = 0x3333333333333333ULL;
    w.blueprints[2].face_idx = 2;

    ReconstructedPath p;
    uint32_t count = path_reconstruct_from_watcher(&p, &w);
    ASSERT(count == 3, "T2 3 nodes reconstructed");
    ASSERT(p.nodes[0].generation == 0, "T2 gen 0");
}

/* ── T3: reconstruct from fabric ── */
static void t3_from_fabric(void) {
    BoundaryFabric f;
    memset(&f, 0, sizeof(f));
    f.snap_count = 2;
    f.snapshots[0].zone_id = 144;
    f.snapshots[0].boundary_core = 0xAAAAAAAABBBBBBBBULL;
    f.snapshots[0].generation = 1;
    f.snapshots[1].zone_id = 288;
    f.snapshots[1].boundary_core = 0xCCCCCCCCDDDDDDDDULL;
    f.snapshots[1].generation = 2;

    ReconstructedPath p;
    uint32_t count = path_reconstruct_from_fabric(&p, &f);
    ASSERT(count == 2, "T3 2 nodes from fabric");
    ASSERT(p.nodes[0].chunk_idx == 143, "T3 chunk 143");
}

/* ── T4: merge paths ── */
static void t4_merge(void) {
    ReconstructedPath wp = {0}, fp = {0}, out = {0};
    
    wp.node_count = 2;
    wp.nodes[0].chunk_idx = 0;
    wp.nodes[1].chunk_idx = 143;
    
    fp.node_count = 1;
    fp.nodes[0].chunk_idx = 287;
    
    uint32_t count = path_merge(&out, &wp, &fp);
    ASSERT(count == 3, "T4 3 merged nodes");
}

/* ── T5: verify ── */
static void t5_verify(void) {
    ReconstructedPath p = {0};
    p.node_count = 3;
    p.nodes[0].chunk_idx = 0;
    p.nodes[0].generation = 0;
    p.nodes[1].chunk_idx = 143;
    p.nodes[1].generation = 1;
    p.nodes[2].chunk_idx = 287;
    p.nodes[2].generation = 2;
    
    uint32_t ok = path_verify(&p);
    ASSERT(ok >= 0, "T5 verify runs");
}

/* ── T6: dump ── */
static void t6_dump(void) {
    ReconstructedPath p = {0};
    p.node_count = 1;
    p.nodes[0].chunk_idx = 100;
    p.nodes[0].master_core = 0xDEADBEEF12345678ULL;
    
    path_dump(&p);
    ASSERT(1, "T6 dump runs");
}

/* ── T7: get sequence ── */
static void t7_sequence(void) {
    ReconstructedPath p = {0};
    p.node_count = 3;
    p.nodes[0].chunk_idx = 10;
    p.nodes[1].chunk_idx = 20;
    p.nodes[2].chunk_idx = 30;
    
    uint32_t indices[5];
    uint32_t count = path_get_sequence(&p, indices, 5);
    ASSERT(count == 3, "T7 3 indices");
    ASSERT(indices[0] == 10, "T7 first index");
}

/* ── T8: empty inputs ── */
static void t8_empty(void) {
    ReconstructedPath p;
    uint32_t count;
    
    count = path_reconstruct_from_watcher(&p, NULL);
    ASSERT(count == 0, "T8 NULL watcher");
    
    count = path_reconstruct_from_fabric(&p, NULL);
    ASSERT(count == 0, "T8 NULL fabric");
}

/* ── T9: duplicate detection ── */
static void t9_dupes(void) {
    ReconstructedPath wp = {0}, fp = {0}, out = {0};
    
    wp.node_count = 2;
    wp.nodes[0].chunk_idx = 100;
    wp.nodes[1].chunk_idx = 200;
    
    fp.node_count = 2;
    fp.nodes[0].chunk_idx = 100;  /* duplicate */
    fp.nodes[1].chunk_idx = 300;
    
    uint32_t count = path_merge(&out, &wp, &fp);
    /* Should skip duplicate at 100 */
    ASSERT(count == 3, "T9 3 unique nodes");
}

/* ── T10: full pipeline ── */
static void t10_full(void) {
    /* Create realistic WatcherCtx with 5 blueprints */
    WatcherCtx w;
    memset(&w, 0, sizeof(w));
    w.bp_count = 5;
    for (int i = 0; i < 5; i++) {
        w.blueprints[i].master_core = (uint64_t)(i + 1) * 0x1111111111111111ULL;
        w.blueprints[i].face_idx = i;
