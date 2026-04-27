/*
 * geo_net.h — GeoNet: Radial Routing Layer
 * ═══════════════════════════════════════════════════════════════════
 *
 * Role: ROUTE (not verify, not store)
 *   value + addr → spoke + slot + mirror_mask → pipeline
 *
 * Stack position:
 *   [L3 Quad]  →  [GeoNet]  →  [geo_cylinder]  →  [Delta/GPU]
 *                                                       ↓
 *                                               [pogls_qrpn verify]
 *                                                       ↓
 *                                          fail → ThirdEye.force_anomaly()
 *
 * Wires:
 *   geo_cylinder.h  — spatial mapping (spoke/slot/face)
 *   geo_thirdeye.h  — adaptive observer (state + mirror mask)
 *
 * Number theory:
 *   3456 = 6 × 576 = 144 × 24  (full space)
 *   576  = 24² = 9 × 64        (slots per spoke)
 *   GROUP_SIZE = 8              (lines per audit group)
 *   AUDIT_DEPTH = 8             (reverse traverse depth)
 *   8 × 8 = 64 = 1 face ✓
 *
 * digit_sum: 3456→9  576→9  144→9  64→1(origin)  8→8
 */

#ifndef GEO_NET_H
#define GEO_NET_H

#include <stdint.h>
#include <string.h>
#include "geo_cylinder.h"
#include "geo_thirdeye.h"

/* ── Constants ─────────────────────────────────────────────────── */
#define GN_GROUP_SIZE    8u    /* Hilbert lines per audit group      */
#define GN_AUDIT_DEPTH   8u    /* reverse traverse depth             */
#define GN_GROUPS_FACE   8u    /* groups per face (64/8)             */
#define GN_LINE_MAX      CYL_FULL_N   /* 3456 total lines            */

/* N4 FIX: Barrett constant สำหรับ mod6 */
#define BARRETT_MOD6_K14  2731U    /* fix: สำหรับ slot→spoke conversion */

/* N3 FIX: dodeca constants */
#define DODECA_PATHS 12    /* 12 dodeca faces */
#define DODECA_WINDOW_LO 96   /* min valid score */
#define DODECA_WINDOW_HI 128  /* max valid score */

/* ── Dodeca stats (N3) ──────────────────────────────────────────── */
typedef struct {
    uint32_t dodeca_score;     /* accumulated score */
    uint32_t valid_paths;      /* count of valid dodeca paths */
    uint8_t  last_verdict;     /* 1=pass, 0=fail */
} DodecaStats;

/* ── Route result ───────────────────────────────────────────────── */
typedef struct {
    uint8_t  spoke;        /* 0..5                                   */
    uint16_t slot;         /* 0..575                                 */
    uint8_t  face;         /* 0..8  (8 = center)                     */
    uint8_t  unit;         /* 0..63 within face                      */
    uint8_t  inv_spoke;    /* mirror: (spoke+3)%6  O(1)              */
    uint8_t  mirror_mask;  /* adaptive: from ThirdEye state          */
    uint8_t  group;        /* line group index (0..7 per face)       */
    uint8_t  is_center;    /* slot >= 512                            */
} GeoNetAddr;

/* ── GeoNet context ─────────────────────────────────────────────── */
typedef struct {
    ThirdEye    eye;          /* adaptive observer                     */
    uint32_t    line_cursor;  /* current Hilbert line position         */
    uint32_t    op_count;     /* total ops routed                      */
    uint32_t    anomaly_signals; /* from pogls_qrpn fail feedback      */
    DodecaStats stats;        /* N3 score tracking                     */
    uint64_t    path_cores[DODECA_PATHS]; /* buffer for score calc     */
} GeoNet;

/* ── Init ───────────────────────────────────────────────────────── */
static inline void geo_net_init(GeoNet *gn, GeoSeed seed) {
    memset(gn, 0, sizeof(GeoNet));
    te_init(&gn->eye, seed);
}

/* ── Core: fast_mod6 (Barrett) ──────────────────────────────────── */
/* N4 FIX: ใช้ k=14 สำหรับ precise conversion */
static inline uint8_t _gn_mod6(uint32_t n) {
    uint32_t q = (n * BARRETT_MOD6_K14) >> 14;
    return (uint8_t)(n - q * 6U);
}

/* ── N3: dodeca score calculation ───────────────────────────────── */
static inline uint32_t _gn_calc_dodeca_score(const uint64_t *cores, uint32_t n) {
    if (!cores || n == 0) return 0;
    uint32_t score = 0;
    for (uint32_t i = 0; i < n && i < DODECA_PATHS; i++) {
        uint32_t pc = (uint32_t)__builtin_popcountll(cores[i]);
        score += (pc / 8);
    }
    return score;
}

static inline uint8_t _gn_dodeca_verdict(uint32_t score) {
    return (score >= DODECA_WINDOW_LO && score <= DODECA_WINDOW_HI) ? 1 : 0;
}

/* ── Route: value+addr → GeoNetAddr ────────────────────────────── */
static inline GeoNetAddr geo_net_route(GeoNet    *gn,
                                        uint64_t   addr,
                                        uint64_t   value,
                                        uint8_t    slot_hot,
                                        GeoSeed    cur)
{
    (void)value;  /* reserved for future value-aware routing */

    uint16_t full_idx = (uint16_t)(addr % CYL_FULL_N);
    uint8_t  spoke    = _gn_mod6(full_idx);
    uint16_t slot     = full_idx / CYL_SPOKES;
    uint8_t  face     = geo_slot_face(slot);
    uint8_t  unit     = geo_slot_unit(slot);
    uint8_t  inv      = geo_spoke_invert(spoke);
    uint8_t  group    = unit / GN_GROUP_SIZE;

    /* ThirdEye tick */
    te_tick(&gn->eye, cur, spoke, slot_hot, 0u);

    /* N3 score tracking: use gen2 as core signal */
    uint32_t path_idx = gn->op_count % DODECA_PATHS;
    gn->path_cores[path_idx] = cur.gen2;
    
    if (path_idx == DODECA_PATHS - 1) {
        gn->stats.dodeca_score = _gn_calc_dodeca_score(gn->path_cores, DODECA_PATHS);
        gn->stats.valid_paths  = DODECA_PATHS;
        gn->stats.last_verdict = _gn_dodeca_verdict(gn->stats.dodeca_score);
    }

    uint8_t mask = te_get_mask(&gn->eye, spoke);

    gn->op_count++;

    return (GeoNetAddr){
        .spoke       = spoke,
        .slot        = slot,
        .face        = face,
        .unit        = unit,
        .inv_spoke   = inv,
        .mirror_mask = mask,
        .group       = group,
        .is_center   = geo_slot_is_center(slot),
    };
}

/* ── Audit group: reverse traverse (neutral wire) ───────────────── */
static inline uint8_t geo_net_is_audit_point(const GeoNetAddr *a) {
    return (a->unit % GN_GROUP_SIZE == GN_GROUP_SIZE - 1) ? 1u : 0u;
}

/* ── QRPN fail feedback → force ThirdEye ANOMALY ────────────────── */
static inline void geo_net_signal_fail(GeoNet *gn) {
    gn->anomaly_signals++;
    gn->eye.cur.hot_slots++;
    if (gn->eye.cur.hot_slots > QRPN_ANOMALY_HOT) {
        gn->eye.qrpn_state = QRPN_ANOMALY;
    }
}

/* ── Force rewind via ThirdEye ───────────────────────────────────── */
static inline GeoSeed geo_net_force_rewind(GeoNet *gn, int steps) {
    return te_rewind(&gn->eye, steps);
}

/* ── State query ────────────────────────────────────────────────── */
static inline uint8_t geo_net_state(const GeoNet *gn) {
    return gn->eye.qrpn_state;
}

static inline const char* geo_net_state_name(const GeoNet *gn) {
    return te_state_name(gn->eye.qrpn_state);
}

/* ── Status ─────────────────────────────────────────────────────── */
static inline void geo_net_status(const GeoNet *gn) {
    printf("[GeoNet] ops=%u  anomaly_signals=%u  state=%s\n",
           gn->op_count, gn->anomaly_signals,
           geo_net_state_name(gn));
    te_status(&gn->eye);
}

#endif /* GEO_NET_H */
