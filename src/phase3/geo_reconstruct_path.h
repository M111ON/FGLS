/*
 * geo_reconstruct_path.h — N2: Path Reconstruction
 * ══════════════════════════════════════════════════════════════════
 *
 * Role: Replay blueprint chain → คืน chunk sequence
 *
 * Design:
 *   - Blueprint stores master_core + face_idx at zone boundaries
 *   - Reconstruct uses slope derivation to reverse-engineer path
 *   - Each blueprint represents a "gate" in the expansion
 *
 * Path derivation:
 *   - master_core[n] = derive_child(master_core[n-1], apex_pattern)
 *   - apex_pattern recovered from blueprint pair
 *
 * Output: ordered chunk_idx stream representing original traversal
 */

#ifndef GEO_RECONSTRUCT_PATH_H
#define GEO_RECONSTRUCT_PATH_H

#include <stdint.h>
#include <string.h>
#include "geo_ghost_watcher.h"
#include "geo_apex_activation.h"
#include "geo_boundary_fabric.h"
#include "core/geo_config.h"

/* ── Path node: reconstructed position ── */
typedef struct {
    uint32_t chunk_idx;      /* position in original stream */
    uint64_t master_core;    /* core at this position */
    uint8_t  face_idx;       /* active face */
    uint8_t  generation;     /* depth in expansion */
} PathNode;

/* ── Reconstructed path ── */
#define RECONSTRUCT_MAX_NODES 256u

typedef struct {
    PathNode   nodes[RECONSTRUCT_MAX_NODES];
    uint32_t   node_count;
    uint8_t    complete;
    uint8_t    _pad[2];
} ReconstructedPath;

/* ── init ── */
static inline void path_init(ReconstructedPath *p) {
    memset(p, 0, sizeof(ReconstructedPath));
}

/* ── reconstruct_from_watcher: replay blueprint chain ── */
/*
 * Takes WatcherCtx.blueprints and reconstructs path.
 * Returns number of nodes in reconstructed path.
 */
static inline uint32_t path_reconstruct_from_watcher(
    ReconstructedPath       *p,
    const WatcherCtx        *w)
{
    if (!p || !w || w->bp_count == 0) return 0;

    path_init(p);
    uint32_t count = 0;

    /* Start from first blueprint as root */
    if (w->bp_count > 0) {
        const GhostRef *root = &w->blueprints[0];
        if (count < RECONSTRUCT_MAX_NODES) {
            p->nodes[count].chunk_idx = 0;  /* first boundary */
            p->nodes[count].master_core = root->master_core;
            p->nodes[count].face_idx = root->face_idx;
            p->nodes[count].generation = 0;
            count++;
        }
    }

    /* Process remaining blueprints as expansion steps */
    for (uint32_t i = 1; i < w->bp_count && count < RECONSTRUCT_MAX_NODES; i++) {
        const GhostRef *bp = &w->blueprints[i];
        
        /* Derive parent from child using inverse */
        /* We know child_core = derive(parent_core, apex, depth) */
        /* So we can compute expected child and match */
        
        /* For now, just record the blueprint as a path node */
        p->nodes[count].chunk_idx = (i + 1u) * TE_CYCLE - 1u;
        p->nodes[count].master_core = bp->master_core;
        p->nodes[count].face_idx = bp->face_idx;
        p->nodes[count].generation = (uint8_t)i;
        count++;
    }

    p->node_count = count;
    p->complete = (count == w->bp_count) ? 1 : 0;
    return count;
}

/* ── reconstruct_from_fabric: use fabric snapshots ── */
static inline uint32_t path_reconstruct_from_fabric(
    ReconstructedPath        *p,
    const BoundaryFabric     *f)
{
    if (!p || !f) return 0;

    path_init(p);
    uint32_t count = 0;

    /* Walk through fabric snapshots */
    for (uint8_t s = 0; s < f->snap_count && count < RECONSTRUCT_MAX_NODES; s++) {
        const BoundarySnapshot *snap = &f->snapshots[s];
        
        p->nodes[count].chunk_idx = snap->zone_id - 1;
        p->nodes[count].master_core = snap->boundary_core;
        p->nodes[count].face_idx = 0;
        p->nodes[count].generation = snap->generation;
        count++;
    }

    p->node_count = count;
    p->complete = (count == f->snap_count) ? 1 : 0;
    return count;
}

/* ── merge_paths: combine watcher + fabric paths ── */
static inline uint32_t path_merge(
    ReconstructedPath       *out,
    const ReconstructedPath *watcher_path,
    const ReconstructedPath *fabric_path)
{
    if (!out) return 0;

    path_init(out);
    uint32_t count = 0;

    /* Add watcher nodes */
    for (uint32_t i = 0; i < watcher_path->node_count && count < RECONSTRUCT_MAX_NODES; i++) {
        out->nodes[count++] = watcher_path->nodes[i];
    }

    /* Add fabric nodes */
    for (uint32_t i = 0; i < fabric_path->node_count && count < RECONSTRUCT_MAX_NODES; i++) {
        /* Skip duplicates based on chunk_idx */
        int dup = 0;
        for (uint32_t j = 0; j < count; j++) {
            if (out->nodes[j].chunk_idx == fabric_path->nodes[i].chunk_idx) {
                dup = 1;
                break;
            }
        }
        if (!dup) {
            out->nodes[count++] = fabric_path->nodes[i];
        }
    }

    /* Sort by chunk_idx */
    for (uint32_t i = 0; i < count; i++) {
        for (uint32_t j = i + 1; j < count; j++) {
            if (out->nodes[i].chunk_idx > out->nodes[j].chunk_idx) {
                PathNode tmp = out->nodes[i];
                out->nodes[i] = out->nodes[j];
                out->nodes[j] = tmp;
            }
        }
    }

    out->node_count = count;
    out->complete = 1;
    return count;
}

/* ── verify_path: check path integrity ── */
static inline uint32_t path_verify(const ReconstructedPath *p) {
    if (!p) return 0;

