/*
 * geo_ops_surface.h — Operational surface for monitor/control
 * ===========================================================
 *
 * Header-only helpers that turn the current phase11 stack into
 * a cleaner operational API for:
 *
 * - ring health snapshots
 * - pyramid readiness / completeness
 * - rewind occupancy
 * - recovery escalation decisions
 * - segment and input validation
 *
 * This file does not replace the existing low-level APIs.
 * It provides a stable place to read stats and derive control
 * decisions without forcing callers to scan multiple headers.
 */

#ifndef GEO_OPS_SURFACE_H
#define GEO_OPS_SURFACE_H

#include <stdint.h>
#include <string.h>

#include "geo_temporal_ring.h"
#include "geo_pyramid.h"
#include "geo_tring_stream.h"
#include "geo_fec.h"
#include "geo_rewind.h"
#include "geo_error.h"

typedef struct {
    uint16_t head;
    uint16_t first_gap;
    uint16_t gap_count_to_head;
    uint16_t residual_total;
    uint16_t missing_accumulated;
    uint32_t chunk_count;
    uint8_t  complete_levels_mask;
} GeoRingStats;

typedef struct {
    uint8_t level;
    uint8_t present;
    uint8_t residual;
    uint8_t complete;
    uint16_t first_gap;
} GeoLevelStats;

typedef struct {
    uint32_t stored_total;
    uint32_t occupied_slots;
    uint16_t capacity_slots;
} GeoRewindStats;

typedef struct {
    GeoRingStats   ring;
    GeoLevelStats  levels[GEO_PYR_DEPTH];
    GeoRewindStats rewind;
} GeoMonitorSnapshot;

typedef struct {
    uint8_t can_flush_level;
    uint8_t should_run_xor;
    uint8_t should_run_rs;
    uint8_t should_rewind_probe;
    uint16_t gaps_in_window;
} GeoRecoveryDecision;

static inline void geo_ops_ring_stats(const TRingCtx *r, GeoRingStats *st)
{
    st->head                = r->head;
    st->first_gap           = tring_first_gap(r);
    st->gap_count_to_head   = tring_gap_count(r, r->head);
    st->residual_total      = pyr_residual_count(r);
    st->missing_accumulated = r->missing;
    st->chunk_count         = r->chunk_count;
    st->complete_levels_mask = pyr_complete_levels(r);
}

static inline void geo_ops_level_stats(const TRingCtx *r,
                                       GeoLevelStats out[GEO_PYR_DEPTH])
{
    PyramidPhase phases[GEO_PYR_DEPTH];
    pyr_scan_all(r, phases);
    for (uint8_t i = 0u; i < GEO_PYR_DEPTH; i++) {
        out[i].level     = phases[i].level;
        out[i].present   = phases[i].present;
        out[i].residual  = phases[i].residual;
        out[i].complete  = phases[i].complete;
        out[i].first_gap = phases[i].first_gap;
    }
}

static inline void geo_ops_rewind_stats(const RewindBuffer *rb, GeoRewindStats *st)
{
    RewindStats raw;
    rewind_stats(rb, &raw);
    st->stored_total   = raw.stored;
    st->occupied_slots = raw.occupied;
    st->capacity_slots = REWIND_SLOTS;
}

static inline void geo_ops_snapshot(const TRingCtx      *r,
                                    const RewindBuffer *rb,
                                    GeoMonitorSnapshot *snap)
{
    geo_ops_ring_stats(r, &snap->ring);
    geo_ops_level_stats(r, snap->levels);
    if (rb) {
        geo_ops_rewind_stats(rb, &snap->rewind);
    } else {
        memset(&snap->rewind, 0, sizeof(snap->rewind));
        snap->rewind.capacity_slots = REWIND_SLOTS;
    }
}

static inline uint8_t geo_ops_level_ready(const TRingCtx *r, uint8_t level)
{
    return (uint8_t)tstream_phase_ready(r, level);
}

static inline uint8_t geo_ops_ring_cycle_complete(const TRingCtx *r)
{
    return (uint8_t)(pyr_residual_count(r) == 0u);
}

static inline uint8_t geo_ops_should_flush_on_tick(const TRingCtx *r)
{
    return (uint8_t)pyr_is_level_transition(r);
}

static inline GeoRecoveryDecision geo_ops_recovery_decide(const TRingCtx      *r,
                                                          uint16_t             n_pkts,
                                                          uint8_t              fec_n,
                                                          const RewindBuffer   *rb)
{
    GeoRecoveryDecision d;
    memset(&d, 0, sizeof(d));

    d.gaps_in_window     = tstream_gap_report(r, n_pkts);
    d.can_flush_level    = (uint8_t)pyr_is_level_transition(r);
    d.should_run_xor     = (uint8_t)(d.gaps_in_window > 0u);
    d.should_run_rs      = (uint8_t)(d.gaps_in_window > fec_n);
    d.should_rewind_probe = (uint8_t)(rb != NULL && d.gaps_in_window > 0u);

    return d;
}

static inline GeoErr geo_ops_validate_ingress(const void *buf,
                                              uint32_t    size,
                                              uint32_t    chunk_sz)
{
    return geo_input_gate(buf, size, chunk_sz);
}

static inline GeoErr geo_ops_validate_segment(uint32_t seg_id, uint32_t seg_total)
{
    return geo_seg_check(seg_id, seg_total);
}

static inline uint8_t geo_ops_whe_alarm(const GeoWhe *w)
{
    return (uint8_t)(!geo_whe_clean(w));
}

#endif /* GEO_OPS_SURFACE_H */

