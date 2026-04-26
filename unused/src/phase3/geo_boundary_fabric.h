/*
 * geo_boundary_fabric.h — M3.3 Boundary Fabric
 * ══════════════════════════════════════════════════════════════════
 *
 * Concept: LC Connector (god's number ≤7) → inter-cube wire
 *          Fibo zone (144) → snapshot boundary ของทั้ง network
 *
 * Network characteristics:
 *   - No central registry — every master knows neighbors via slope derivation
 *   - Lazy: doesn't exist until traversed
 *   - LC Connector: wire with ≤7 segments (god's number)
 *   - Fibo zone: hard boundary at zone 720 = full flush
 *
 * LC Connector rules (LOCKED):
 *   - god's number = 7 (maximum segments for optimal path)
 *   - Inter-cube wire connects adjacent masters
 *   - Each master has up to 6 neighbors (one per face)
 *   - Wire segments are derived, not stored
 *
 * Fibo zone boundaries:
 *   - Zone 144: snapshot boundary (1 TE_CYCLE)
 *   - Zone 288: 2 cycles
 *   - Zone 432: 3 cycles
 *   - Zone 576: 4 cycles
 *   - Zone 720: full flush (5 cycles = complete network)
 */

#ifndef GEO_BOUNDARY_FABRIC_H
#define GEO_BOUNDARY_FABRIC_H

#include <stdint.h>
#include <string.h>
#include "geo_expansion_topology.h"  /* ExpansionNetwork, MasterNode */
#include "geo_apex_activation.h"      /* ApexRef, apex_derive_child */
#include "core/geo_config.h"         /* TE_CYCLE, GEO_SPOKES */
#include "core/geo_thirdeye.h"        /* ThirdEye */

/* ── LC Connector constants (LOCKED) ── */
#define LC_GODS_NUMBER     7u    /* maximum wire segments */
#define LC_MAX_WIRES       64u   /* wires per fabric */
#define LC_MAX_SEGMENTS    6u    /* segments per wire (faces) */

/* ── Wire segment ── */
typedef struct {
    uint16_t from_node;    /* source master index */
    uint16_t to_node;      /* target master index */
    uint8_t  face;         /* face used for connection */
    uint8_t  hops;         /* wire segments to reach (≤ LC_GODS_NUMBER) */
} WireSegment;

/* ── LC Connector: inter-cube wire ── */
typedef struct {
    uint32_t     wire_id;       /* unique wire identifier */
    WireSegment  segments[LC_MAX_SEGMENTS];
    uint8_t      seg_count;    /* active segments */
    uint8_t      active;       /* 1 = connected, 0 = dormant */
    uint8_t      _pad[2];
} LCConnector;

/* ── Fibo zone boundaries (LOCKED) ── */
#define FIBO_ZONE_144    144u
#define FIBO_ZONE_288    288u
#define FIBO_ZONE_432    432u
#define FIBO_ZONE_576    576u
#define FIBO_ZONE_720    720u   /* full flush */

/* ── Boundary state ── */
#define BOUNDARY_DORMANT    0
#define BOUNDARY_WIRED      1
#define BOUNDARY_SNAPSHOT   2
#define BOUNDARY_FLUSHED    3

/* ── BoundarySnapshot: zone boundary record ── */
typedef struct {
    uint32_t    zone_id;       /* 144, 288, 432, 576, 720 */
    uint64_t    boundary_core; /* master_core at boundary */
    uint32_t    master_count;  /* number of masters in network */
    uint32_t    wire_count;    /* number of active wires */
    uint8_t     generation;    /* max generation at boundary */
    uint8_t     _pad[3];
} BoundarySnapshot;   /* 24B */

/* ── BoundaryFabric: full network boundary tracker ── */
#define BOUNDARY_MAX_SNAPSHOTS 5u   /* zones 144/288/432/576/720 */

typedef struct {
    ExpansionNetwork  network;     /* the lazy lattice */
    LCConnector       wires[LC_MAX_WIRES];
    uint32_t          wire_count;
    BoundarySnapshot  snapshots[BOUNDARY_MAX_SNAPSHOTS];
    uint8_t           snap_count;
    uint8_t           state;
    uint8_t           _pad[2];
    uint64_t          zone_counter;
} BoundaryFabric;

/* ── init: create fabric with genesis master ── */
static inline void fabric_init(BoundaryFabric *f, uint64_t genesis_core) {
    memset(f, 0, sizeof(BoundaryFabric));
    expansion_init(&f->network, genesis_core);
    f->wire_count   = 0;
    f->snap_count   = 0;
    f->state         = BOUNDARY_DORMANT;
    f->zone_counter  = 0;
}

/* ── wire_create: create LC connector between two masters ── */
/*
 * Returns wire index, or 0xFFFFFFFF if failed.
 * Wire is derived from slope — no stored path.
 */
static inline uint32_t fabric_wire_create(BoundaryFabric *f,
                                           uint16_t        from_idx,
                                           uint16_t        to_idx,
                                           uint8_t         face)
{
    if (f->wire_count >= LC_MAX_WIRES) return 0xFFFFFFFF;
    if (from_idx >= f->network.node_count) return 0xFFFFFFFF;
    if (to_idx >= f->network.node_count) return 0xFFFFFFFF;

    /* check face validity */
    if (face >= 6) return 0xFFFFFFFF;

    /* check complement pair (no entanglement) */
    const MasterNode *from = &f->network.nodes[from_idx];
    if (is_complement_pair(face, from->face_activated)) return 0xFFFFFFFF;

    /* compute slope-based wire */
    uint64_t slope_from = apex_slope(from->master_core, face);
    const MasterNode *to = &f->network.nodes[to_idx];
    uint64_t slope_to = apex_slope(to->master_core, (face + 3) % 6);

    /* verify slope match (bi-directional) */
    if (slope_from != slope_to) {
        printf("[DEBUG] slope mismatch: from=0x%lx to=0x%lx face=%d\n", (unsigned long)slope_from, (unsigned long)slope_to, face);
        return 0xFFFFFFFF;
    }

    /* create wire */
    uint32_t wire_idx = f->wire_count++;
    LCConnector *wire = &f->wires[wire_idx];
    wire->wire_id    = wire_idx;
    wire->active     = 1;

    /* single segment (direct neighbor) */
    wire->segments[0].from_node = from_idx;
    wire->segments[0].to_node   = to_idx;
    wire->segments[0].face      = face;
    wire->segments[0].hops      = 1;
    wire->seg_count             = 1;

    f->state = BOUNDARY_WIRED;
    return wire_idx;
}

/* ── wire_discover: auto-discover all valid wires ── */
/*
 * Scans network for valid slope matches between all node pairs.
 * Lazy: only creates wires when slope derivation is valid.
 */
static inline uint32_t fabric_wire_discover(BoundaryFabric *f) {
    uint32_t wired = 0;

    for (uint16_t i = 0; i < f->network.node_count; i++) {
        for (uint16_t j = i + 1; j < f->network.node_count; j++) {
            for (uint8_t face = 0; face < 6; face++) {
                uint32_t wire = fabric_wire_create(f, i, j, face);
                if (wire != 0xFFFFFFFF) wired++;
            }
        }
    }

    return wired;
}

/* ── tick: advance zone counter, capture snapshot at boundaries ── */
/*
 * Called per chunk traversal.
 * Captures boundary snapshot at zones 144/288/432/576/720.
 * Returns zone_id if boundary crossed, 0 otherwise.
 */
static inline uint32_t fabric_tick(BoundaryFabric *f) {
    f->zone_counter++;

    /* check Fibo zone boundaries */
    static const uint32_t zones[] = {144, 288, 432, 576, 720};
    for (int i = 0; i < 5; i++) {
        if (f->zone_counter == zones[i]) {
            /* capture snapshot */
            if (f->snap_count < BOUNDARY_MAX_SNAPSHOTS) {
                BoundarySnapshot *snap = &f->snapshots[f->snap_count++];
                snap->zone_id       = zones[i];
                snap->boundary_core = f->network.nodes[0].master_core;
                snap->master_count  = f->network.node_count;
                snap->generation    = (uint8_t)i;
            }
            return 1;
        }
    }
    return 0;
}

/* ── fabric_expand: grow the boundary network ── */
static inline uint16_t fabric_expand(BoundaryFabric *f,
                                      uint16_t        parent_idx,
                                      uint8_t         activated_face)
{
    return expansion_expand(&f->network, parent_idx, activated_face);
}

/* ── fabric_get_snapshot: retrieve snapshot by zone ── */
static inline const BoundarySnapshot* fabric_get_snapshot(const BoundaryFabric *f, uint32_t zone_id) {
    for (uint8_t i = 0; i < f->snap_count; i++) {
        if (f->snapshots[i].zone_id == zone_id) return &f->snapshots[i];
    }
    return NULL;
}

/* ── fabric_verify: verify full fabric integrity ── */
static inline uint32_t fabric_verify(const BoundaryFabric *f) {
    uint32_t ok = 0;
    /* Basic: check all nodes have valid cores */
    for (uint32_t i = 0; i < f->network.node_count; i++) {
        if (f->network.nodes[i].master_core != 0) ok++;
    }
    return ok;
}

/* ── fabric_status: report fabric health ── */
static inline void fabric_status(const BoundaryFabric *f) {
    printf("[Fabric] masters=%u  wires=%u  snapshots=%u  state=%d\n",
           f->network.node_count, f->wire_count, f->snap_count, f->state);
}

#endif /* GEO_BOUNDARY_FABRIC_H */

