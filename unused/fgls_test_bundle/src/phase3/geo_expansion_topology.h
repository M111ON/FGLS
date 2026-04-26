/*
 * geo_expansion_topology.h — M3.2 Expansion Topology
 * ══════════════════════════════════════════════════════════════════
 *
 * Concept: Lazy fractal cube lattice
 *   - Generation 0: [Master₀]
 *   - Generation 1: [Master₀] ── [Master₁]  (face 0 activated)
 *   - Generation 2: [Master₀] ── [Master₁] ── [Master₂]  (face 2 activated)
 *
 * Network structure:
 *   - Lazy expansion: nodes only exist when traversed
 *   - No central registry — each master knows neighbors via slope derivation
 *   - LC Connector (god's number ≤7) → inter-cube wire
 *   - Fibo zone (144) → snapshot boundary of entire network
 *
 * Topology Rules (LOCKED):
 *   - master_core → ghost derivation = pure function
 *   - No heap allocation — all pointers are indices into fixed arrays
 *   - Every expansion step must pass through Fibo zone gate
 *   - Boundary at zone 720 = full network flush
 *
 * Memory model:
 *   - Fixed-size array of MasterNode (no realloc)
 *   - Neighbor discovery via slope, not pointer chase
 */

#ifndef GEO_EXPANSION_TOPOLOGY_H
#define GEO_EXPANSION_TOPOLOGY_H

#include <stdint.h>
#include <string.h>
#include "geo_apex_activation.h"   /* ApexRef, apex_derive_child */
#include "core/geo_config.h"        /* TE_CYCLE, GEO_SPOKES */
#include "core/geo_thirdeye.h"      /* ThirdEye */

/* ── Generation limits ── */
#define EXPANSION_MAX_DEPTH    4u   /* max generation depth */
#define EXPANSION_MAX_MASTERS  64u  /* max masters per network */
#define EXPANSION_MAX_NEIGHBORS 6u  /* max neighbors per master (one per face) */

/* ── MasterNode: a single node in the expansion lattice ── */
typedef struct {
    uint64_t master_core;          /* core fingerprint                */
    uint8_t  generation;           /* 0 = root, 1 = child, ...        */
    uint8_t  face_activated;      /* which face triggered expansion  */
    uint8_t  neighbor_count;      /* populated neighbors             */
    uint8_t  _pad;
    uint16_t neighbors[EXPANSION_MAX_NEIGHBORS];  /* indices into node array */
    uint16_t parent_idx;          /* index of parent node            */
} MasterNode;   /* 40B */

/* ── Network: lazy expansion context ── */
typedef struct {
    MasterNode   nodes[EXPANSION_MAX_MASTERS];
    uint32_t     node_count;
    uint8_t      max_generation;
    uint8_t      state;           /* EXPANSION_* */
    uint8_t      _pad[2];
    ThirdEye     te;              /* field observer for boundary     */
    uint64_t     zone_counter;    /* Fibo zone progress             */
} ExpansionNetwork;

/* ── Expansion states ── */
#define EXPANSION_DORMANT   0
#define EXPANSION_GROWING   1
#define EXPANSION_STABLE    2
#define EXPANSION_FLUSHED   3

/* ── init: create generation 0 root ── */
static inline void expansion_init(ExpansionNetwork *net, uint64_t root_core) {
    memset(net, 0, sizeof(ExpansionNetwork));
    
    /* create root node at generation 0 */
    MasterNode *root = &net->nodes[0];
    root->master_core      = root_core;
    root->generation       = 0;
    root->face_activated  = 0xFF;  /* 0xFF = root (no parent) */
    root->neighbor_count  = 0;
    root->parent_idx      = 0xFFFF;  /* none */
    
    net->node_count       = 1;
    net->max_generation   = 0;
    net->state            = EXPANSION_DORMANT;
    net->zone_counter     = 0;
}

/* ── expand: add new master at generation+1 ── */
/*
 * Called when face activation triggers expansion.
 * Derives new master_core from parent via apex_derive_child().
 * Returns new node index, or 0xFFFF if failed.
 */
static inline uint16_t expansion_expand(ExpansionNetwork *net,
                                         uint16_t         parent_idx,
                                         uint8_t          activated_face)
{
    if (net->node_count >= EXPANSION_MAX_MASTERS) return 0xFFFF;
    if (parent_idx >= net->node_count) return 0xFFFF;

    MasterNode *parent = &net->nodes[parent_idx];
    if (parent->generation >= EXPANSION_MAX_DEPTH) return 0xFFFF;

    /* derive child core */
    uint64_t child_core = apex_derive_child(parent->master_core,
                                             (uint64_t)activated_face,
                                             parent->generation + 1);

    /* check for duplicate (lazy dedup) */
    for (uint32_t i = 0; i < net->node_count; i++) {
        if (net->nodes[i].master_core == child_core) return 0xFFFF;
    }

    /* create new node */
    uint16_t new_idx = net->node_count++;
    MasterNode *child = &net->nodes[new_idx];
    child->master_core      = child_core;
    child->generation       = parent->generation + 1;
    child->face_activated   = activated_face;
    child->neighbor_count   = 0;
    child->parent_idx       = parent_idx;

    /* link to parent */
    if (parent->neighbor_count < EXPANSION_MAX_NEIGHBORS) {
        parent->neighbors[parent->neighbor_count++] = new_idx;
    }

    /* update max generation */
    if (child->generation > net->max_generation) {
        net->max_generation = child->generation;
    }

    net->state = EXPANSION_GROWING;
    return new_idx;
}

/* ── neighbor_discover: derive neighbor via slope ── */
/*
 * Given a master node, discover its neighbors by computing
 * slopes on all 6 faces and checking for valid connections.
 * Neighbor = master whose core matches slope-derived expectation.
 *
 * This is LAZY: neighbors are computed on-demand, not stored.
 */
static inline void expansion_neighbor_discover(const ExpansionNetwork *net,
                                                uint16_t               node_idx,
                                                uint16_t              *out_neighbors,
                                                uint8_t               *out_count)
{
    if (node_idx >= net->node_count) { *out_count = 0; return; }

    const MasterNode *node = &net->nodes[node_idx];
    *out_count = 0;

    for (uint8_t f = 0; f < 6 && *out_count < EXPANSION_MAX_NEIGHBORS; f++) {
        /* skip complement pairs (entanglement) */
        if (is_complement_pair(f, node->face_activated)) continue;

        uint64_t slope_f = apex_slope(node->master_core, f);

        /* search for matching slope in other nodes */
        for (uint32_t i = 0; i < net->node_count; i++) {
            if (i == node_idx) continue;  /* skip self */

            const MasterNode *other = &net->nodes[i];
            /* check if other node's core produces matching slope */
            uint64_t other_slope = apex_slope(other->master_core, (f + 3) % 6);
            if (slope_f == other_slope) {
                out_neighbors[(*out_count)++] = (uint16_t)i;
                break;  /* one neighbor per face */
            }
        }
    }
}

/* ── tick: advance zone counter, check Fibo boundaries ── */
/*
 * Called per chunk traversal.
 * Returns 1 if zone boundary crossed (zone 144/720/...).
 */
static inline int expansion_tick(ExpansionNetwork *net) {
    net->zone_counter++;

    /* Fibo zone boundaries: 144, 288, 432, 576, 720, ... */
    if (net->zone_counter % TE_CYCLE == 0) {
        /* crossed a Fibo zone */
        if (net->zone_counter >= 720) {
            net->state = EXPANSION_FLUSHED;
        } else if (net->state == EXPANSION_GROWING) {
            net->state = EXPANSION_STABLE;
        }
        return 1;  /* zone boundary */
    }
    return 0;
}

#endif /* GEO_EXPANSION_TOPOLOGY_H */

