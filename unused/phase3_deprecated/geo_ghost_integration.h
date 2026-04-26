/*
 * geo_ghost_integration.h — N1: Scanner × Watcher × Apex Wire
 * ══════════════════════════════════════════════════════════════════
 *
 * Role: Full pipeline integration
 *   scan_buf → GhostScanCtx → WatcherCtx → ApexCtx → BoundaryFabric
 *
 * This is the REAL wire that connects scanner callback to expansion.
 *
 * Wire flow:
 *   1. scan_buf() emits ScanEntry per chunk
 *   2. ghost_scan_cb() → watcher_feed() records blueprints
 *   3. Apex activation detected in parallel
 *   4. Fabric expands on apex events
 *
 * Design:
 *   - No heap allocation
 *   - All components stack-allocated
 *   - Callback-driven for streaming
 */

#ifndef GEO_GHOST_INTEGRATION_H
#define GEO_GHOST_INTEGRATION_H

#include <stdint.h>
#include <string.h>
#include "core/pogls_scanner.h"
#include "geo_ghost_watcher.h"
#include "geo_ghost_scanner.h"
#include "geo_apex_activation.h"
#include "geo_boundary_fabric.h"
#include "core/geo_config.h"
#include "core/geo_thirdeye.h"

/* ── Integration config ── */
#define GHOST_INTEG_MAX_EXPANSION_EVENTS 32u

/* ── Expansion event ── */
typedef struct {
    uint16_t parent_idx;
    uint8_t  activated_face;
    uint64_t timestamp;
} ExpansionEvent;

/* ── Integrated context: all components in one ── */
typedef struct {
    /* Core components */
    WatcherCtx     watcher;
    GhostScanCtx   scanner;
    ApexCtx        apex;
    BoundaryFabric fabric;

    /* Wiring state */
    ExpansionEvent expansion_events[GHOST_INTEG_MAX_EXPANSION_EVENTS];
    uint16_t       expansion_count;

    /* Stats */
    uint64_t       total_chunks;
    uint64_t       total_apex_activations;
    uint32_t       global_step;           /* sequence index in expansion chain */
    uint8_t        init_ok;
    uint8_t        _pad[3];
} GhostIntegration;

/* ── init: bootstrap full pipeline ── */
static inline void ghost_integ_init(GhostIntegration *gi, uint64_t genesis_core) {
    memset(gi, 0, sizeof(GhostIntegration));

    /* init all components with same genesis */
    GeoSeed genesis = { genesis_core, 0 };
    watcher_init(&gi->watcher, genesis);
    ghost_scan_init(&gi->scanner, &gi->watcher);
    apex_init(&gi->apex, genesis);
    fabric_init(&gi->fabric, genesis_core);

    gi->init_ok = 1;
    gi->total_chunks = 0;
    gi->total_apex_activations = 0;
    gi->expansion_count = 0;
    gi->global_step = 0;
}

/* ── scan_callback: the REAL callback for scan_buf ── */
/*
 * This is the core wire: per-chunk callback that feeds all components.
 * 
 * Flow per chunk:
 *   1. Update watcher (blueprint recording)
 *   2. Update apex (activation detection)
 *   3. Check for expansion events
 *   4. Tick fabric boundaries
 */
static inline void ghost_scan_callback(const ScanEntry *e, void *user) {
    GhostIntegration *gi = (GhostIntegration *)user;
    if (!gi || !gi->init_ok) return;

    gi->total_chunks++;

    uint64_t core = e->seed;
    uint8_t  face = (uint8_t)(e->coord.face % 6);
    uint8_t  slot_hot = 1;  /* S35 lock */

    /* 1. Feed watcher (blueprint recording) */
    watcher_feed(&gi->watcher, core, face, slot_hot);

    /* 2. Feed apex (activation detection) */
    uint8_t apex_state = apex_feed(&gi->apex, core, face, slot_hot, gi->global_step);
    if (apex_state == APEX_ACTIVE) {
        gi->total_apex_activations++;

        /* Queue expansion event */
        if (gi->expansion_count < GHOST_INTEG_MAX_EXPANSION_EVENTS) {
            ExpansionEvent *ev = &gi->expansion_events[gi->expansion_count++];
            ev->parent_idx = 0;  /* root for now */
            ev->activated_face = face;
            ev->timestamp = gi->total_chunks;
            
            /* CRITICAL: Overwrite parent_core in ApexRef to be the genesis core
               to ensure 'seed + faces only' reconstruction works. */
            if (gi->apex.act_count > 0) {
                gi->apex.activations[gi->apex.act_count - 1].parent_core = gi->watcher.te.genesis.gen2;
            }
        }
    }

    /* 3. Tick fabric boundary */
    fabric_tick(&gi->fabric);

    /* 4. Process expansion if zone crossed */
    if (gi->total_chunks % TE_CYCLE == 0) {
        /* Process queued expansion events */
        for (uint16_t i = 0; i < gi->expansion_count; i++) {
            ExpansionEvent *ev = &gi->expansion_events[i];
            fabric_expand(&gi->fabric, ev->parent_idx, ev->activated_face);
        }
        gi->expansion_count = 0;  /* flush queue */
    }
}

/* ── scan_and_expand: convenience wrapper ── */
static inline uint32_t ghost_scan_and_expand(
    const uint8_t  *buf,
    size_t          len,
    uint64_t        genesis_core,
    const ScanConfig *cfg)
{
    GhostIntegration gi;
    ghost_integ_init(&gi, genesis_core);

    return scan_buf(buf, len, ghost_scan_callback, &gi, cfg);
}

/* ── get_stats: retrieve pipeline stats ── */
static inline void ghost_integ_stats(const GhostIntegration *gi) {
    printf("[Integration] chunks=%llu blueprints=%u apex_events=%llu\n",
           (unsigned long long)gi->total_chunks,
           gi->watcher.bp_count,
           (unsigned long long)gi->total_apex_activations);
    printf("  network: %u masters, %u wires\n",
           gi->fabric.network.node_count,
           gi->fabric.wire_count);
    watcher_status(&gi->watcher);
    apex_status(&gi->apex);
    fabric_status(&gi->fabric);
}

/* ── verify: check all component integrity ── */
static inline uint32_t ghost_integ_verify(const GhostIntegration *gi) {
    uint32_t ok = 0;

    /* Verify watcher */
    ok += watcher_verify(&gi->watcher);

    /* Verify apex */
    ok += apex_verify(&gi->apex);

    /* Verify fabric */
    ok += fabric_verify(&gi->fabric);

    return ok;
}

#endif /* GEO_GHOST_INTEGRATION_H */
