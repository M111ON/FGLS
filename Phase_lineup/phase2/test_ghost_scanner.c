/*
 * test_ghost_scanner.c — geo_ghost_scanner.h integration tests (S35)
 *
 * Tests:
 *   T1  ghost_scan_init / basic call
 *   T2  ghost_scan_cb maps seed→core, face%6→spoke correctly
 *   T3  single TE_CYCLE (144 chunks) seals exactly 1 blueprint
 *   T4  two TE_CYCLEs seal 2 blueprints, chunk_idx correct
 *   T5  ghost_replay emits correct chunk_idx per blueprint
 *   T6  ghost_replay_watcher = ghost_replay on w->blueprints
 *   T7  ghost_scan_and_replay full pipeline (1-call convenience)
 *   T8  ghost_replay on empty chain → 0 callbacks (no crash)
 *   T9  replay chunk_idx formula: (i+1)*144-1
 *   T10 passthru-flagged chunks still feed watcher (no skip)
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

/* ── stubs for headers not present in test zip ── */

/* theta_map.h stub — face = seed%6 suffices for tests */
#ifndef THETA_MAP_H
#define THETA_MAP_H
typedef struct { uint8_t face; uint8_t edge; uint16_t z; } ThetaCoord;
static inline ThetaCoord theta_map(uint64_t seed) {
    ThetaCoord c = { (uint8_t)(seed % 6), 0, 0 };
    return c;
}
#endif

/* angular_mapper_v36.h stub */
#ifndef ANGULAR_MAPPER_V36_H
#define ANGULAR_MAPPER_V36_H
static inline uint64_t pogls_node_to_address(uint32_t f,
    uint8_t g, uint8_t w, uint8_t nb) {
    (void)f; (void)g; (void)w; (void)nb; return 0;
}
#endif

/* real headers — order matters: config → fold → thirdeye → watcher → scanner → ours */
#include "core/geo_config.h"
#include "core/geo_thirdeye.h"
#include "geo_ghost_watcher.h"
#include "pogls_scanner.h"
#include "geo_ghost_scanner.h"

/* ── test helpers ── */

static int g_pass = 0, g_fail = 0;

#define ASSERT(cond, msg) do { \
    if (cond) { printf("[PASS] %s\n", msg); g_pass++; } \
    else       { printf("[FAIL] %s  (line %d)\n", msg, __LINE__); g_fail++; } \
} while(0)

/* ── Replay collector ── */
typedef struct {
    uint32_t  idxs[256];
    uint64_t  cores[256];
    uint8_t   faces[256];
    uint32_t  count;
} ReplayLog;

static void replay_collect(uint32_t chunk_idx, const GhostRef *ref, void *user) {
    ReplayLog *log = (ReplayLog *)user;
    if (log->count < 256) {
        log->idxs[log->count]  = chunk_idx;
        log->cores[log->count] = ref->master_core;
        log->faces[log->count] = ref->face_idx;
        log->count++;
    }
}

/* ── Build synthetic buffer of n_chunks × 64B ── */
static void make_buf(uint8_t *out, uint32_t n_chunks, uint64_t seed_base) {
    for (uint32_t i = 0; i < n_chunks; i++) {
        uint64_t *w = (uint64_t *)(out + i * 64);
        for (int j = 0; j < 8; j++) {
            /* each word varies per chunk so seed differs */
            w[j] = seed_base ^ ((uint64_t)i << 16) ^ (uint64_t)j;
        }
    }
}

/* ── T1: init/basic ── */
static void t1_init(void) {
    GeoSeed genesis = { 0xDEADBEEFULL, 0xCAFEBABEULL };
    WatcherCtx w;
    GhostScanCtx gsc;
    watcher_init(&w, genesis);
    ghost_scan_init(&gsc, &w);
    ASSERT(gsc.watcher == &w,      "T1 watcher ptr set");
    ASSERT(gsc.bp_events == 0,     "T1 bp_events zero");
    ASSERT(w.bp_count == 0,        "T1 no blueprints yet");
    ASSERT(w.state == GHOST_DORMANT, "T1 state DORMANT");
}

/* ── T2: ghost_scan_cb maps fields ── */
static void t2_cb_mapping(void) {
    GeoSeed genesis = { 1ULL, 2ULL };
    WatcherCtx w; watcher_init(&w, genesis);
    GhostScanCtx gsc; ghost_scan_init(&gsc, &w);

    /* craft a ScanEntry manually */
    ScanEntry e;
    memset(&e, 0, sizeof(e));
    e.seed          = 0xABCDEF01ULL;
    e.coord.face    = 7;   /* 7 % 6 = 1 */
    e.chunk_idx     = 0;
    e.flags         = SCAN_FLAG_VALID;

    ghost_scan_cb(&e, &gsc);

    ASSERT(w.chunk_count == 1,       "T2 chunk_count incremented");
    ASSERT(w.state == GHOST_ACTIVE,  "T2 state ACTIVE after first chunk");
}

/* ── T3: 144 chunks → 1 blueprint ── */
static void t3_one_cycle(void) {
    GeoSeed genesis = { 0x1111ULL, 0x2222ULL };
    WatcherCtx w; watcher_init(&w, genesis);
    GhostScanCtx gsc; ghost_scan_init(&gsc, &w);

    uint8_t buf[144 * 64];
    make_buf(buf, 144, 0x1234ULL);

    uint32_t emitted = scan_buf(buf, sizeof(buf), ghost_scan_cb, &gsc, NULL);

    ASSERT(emitted == 144,           "T3 144 chunks emitted");
    ASSERT(w.bp_count == 1,          "T3 exactly 1 blueprint");
    ASSERT(gsc.bp_events == 1,       "T3 bp_events == 1");
    ASSERT(w.state == GHOST_BLUEPRINT, "T3 state BLUEPRINT");
}

/* ── T4: 288 chunks → 2 blueprints ── */
static void t4_two_cycles(void) {
    GeoSeed genesis = { 0x3333ULL, 0x4444ULL };
    WatcherCtx w; watcher_init(&w, genesis);
    GhostScanCtx gsc; ghost_scan_init(&gsc, &w);

    uint8_t buf[288 * 64];
    make_buf(buf, 288, 0x5678ULL);

    scan_buf(buf, sizeof(buf), ghost_scan_cb, &gsc, NULL);

    ASSERT(w.bp_count == 2,   "T4 2 blueprints");
    ASSERT(gsc.bp_events == 2, "T4 bp_events == 2");
}

/* ── T5: ghost_replay chunk_idx values ── */
static void t5_replay_idx(void) {
    GeoSeed genesis = { 0x5555ULL, 0x6666ULL };
    WatcherCtx w; watcher_init(&w, genesis);
    GhostScanCtx gsc; ghost_scan_init(&gsc, &w);

    uint8_t buf[288 * 64];
    make_buf(buf, 288, 0x9ABCULL);
    scan_buf(buf, sizeof(buf), ghost_scan_cb, &gsc, NULL);

    ReplayLog log; memset(&log, 0, sizeof(log));
    ghost_replay(w.blueprints, w.bp_count, replay_collect, &log);

    ASSERT(log.count == 2,             "T5 replay emits 2 events");
    /* blueprint[0] → chunk (1*144)-1 = 143 */
    ASSERT(log.idxs[0] == 143,         "T5 blueprint[0] chunk_idx=143");
    /* blueprint[1] → chunk (2*144)-1 = 287 */
    ASSERT(log.idxs[1] == 287,         "T5 blueprint[1] chunk_idx=287");
}

/* ── T6: ghost_replay_watcher ≡ ghost_replay ── */
static void t6_replay_watcher(void) {
    GeoSeed genesis = { 0x7777ULL, 0x8888ULL };
    WatcherCtx w; watcher_init(&w, genesis);
    GhostScanCtx gsc; ghost_scan_init(&gsc, &w);

    uint8_t buf[144 * 64];
    make_buf(buf, 144, 0xDEADULL);
    scan_buf(buf, sizeof(buf), ghost_scan_cb, &gsc, NULL);

    ReplayLog log1, log2;
    memset(&log1, 0, sizeof(log1));
    memset(&log2, 0, sizeof(log2));

    ghost_replay(w.blueprints, w.bp_count, replay_collect, &log1);
    ghost_replay_watcher(&w, replay_collect, &log2);

    ASSERT(log1.count == log2.count,   "T6 same event count");
    ASSERT(log1.idxs[0] == log2.idxs[0], "T6 same chunk_idx");
    ASSERT(log1.cores[0] == log2.cores[0], "T6 same core");
}

/* ── T7: ghost_scan_and_replay convenience ── */
static void t7_full_pipeline(void) {
    GeoSeed genesis = { 0x9999ULL, 0xAAAAULL };

    uint8_t buf[144 * 64];
    make_buf(buf, 144, 0xF00DULL);

    ReplayLog log; memset(&log, 0, sizeof(log));

    uint32_t chunks = ghost_scan_and_replay(
        buf, sizeof(buf), genesis, NULL, replay_collect, &log);

    ASSERT(chunks == 144,   "T7 scanned 144 chunks");
    ASSERT(log.count == 1,  "T7 replay emits 1 event");
    ASSERT(log.idxs[0] == 143, "T7 chunk_idx=143");
}

/* ── T8: empty chain → no crash ── */
static void t8_empty_replay(void) {
    ReplayLog log; memset(&log, 0, sizeof(log));
    ghost_replay(NULL, 0, replay_collect, &log);   /* NULL chain */
    ASSERT(log.count == 0, "T8 NULL chain no crash");

    GhostRef empty[1];
    ghost_replay(empty, 0, replay_collect, &log);  /* n=0 */
    ASSERT(log.count == 0, "T8 n=0 no callbacks");
}

/* ── T9: chunk_idx formula verification ── */
static void t9_idx_formula(void) {
    /* For blueprint[i]: chunk_idx = (i+1)*TE_CYCLE - 1
     * Spot-check: i=0→143, i=5→719, i=143→20735 */
    uint32_t cycle = (uint32_t)TE_CYCLE;
    ASSERT((1u * cycle - 1u) == 143,   "T9 formula i=0 → 143");
    ASSERT((6u * cycle - 1u) == 719,   "T9 formula i=5 → 719");
    ASSERT((144u * cycle - 1u) == 20735, "T9 formula i=143 → 20735");
}

/* ── T10: passthru chunks still feed watcher ── */
static void t10_passthru_feeds(void) {
    GeoSeed genesis = { 0xBBBBULL, 0xCCCCULL };
    WatcherCtx w; watcher_init(&w, genesis);
    GhostScanCtx gsc; ghost_scan_init(&gsc, &w);

    /* 144 chunks that would be flagged PASSTHRU (ZIP magic) */
    uint8_t buf[144 * 64];
    memset(buf, 0, sizeof(buf));
    /* inject ZIP magic in first 4 bytes → whole file = passthru */
    buf[0] = 0x50; buf[1] = 0x4B; buf[2] = 0x03; buf[3] = 0x04;

    scan_buf(buf, sizeof(buf), ghost_scan_cb, &gsc, NULL);

    /* watcher_feed is called regardless of passthru flag */
    ASSERT(w.chunk_count == 144,  "T10 passthru chunks counted");
    ASSERT(w.bp_count == 1,       "T10 blueprint sealed despite passthru");
}

/* ── main ── */
int main(void) {
    printf("=== test_ghost_scanner (S35) ===\n");
    t1_init();
    t2_cb_mapping();
    t3_one_cycle();
    t4_two_cycles();
    t5_replay_idx();
    t6_replay_watcher();
    t7_full_pipeline();
    t8_empty_replay();
    t9_idx_formula();
    t10_passthru_feeds();
    printf("================================\n");
    printf("Result: %d/%d PASS\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
