/*
 * geo_reconstruct_path.h — N2: Path Reconstruction
 * ══════════════════════════════════════════════════════════════════
 *
 * Role: Replay expansion chain → คืน core sequence
 *
 * Design (Stateless):
 *   - "Blueprint = implicit function"
 *   - Reconstruct from (seed + face sequence + step)
 *   - Zero storage overhead
 */

#ifndef GEO_RECONSTRUCT_PATH_H
#define GEO_RECONSTRUCT_PATH_H

#include <stdint.h>
#include <string.h>
#include "core/geo_primitives.h"
#include "geo_ghost_watcher.h"
#include "phase3/geo_boundary_fabric.h"
#include "core/geo_config.h"

/* ── ghost_shadow: O(1) verification without extra storage ── */
static inline uint64_t ghost_shadow(uint64_t core, uint8_t face, uint32_t step) {
    return _mix64(core ^ 0xA5A5A5A5 ^ face ^ step);
}

/* ── verify_step: check chain integrity ── */
static inline int verify_step(uint64_t parent, uint64_t child, uint8_t face, uint32_t step) {
    uint64_t expected = derive_next_core(parent, face, step);
    return (expected == child);
}

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

/* ── reconstruct_chain: replay from seed + faces ── */
static inline void reconstruct_chain(uint64_t seed,
                                    const uint8_t *faces,
                                    uint32_t n,
                                    uint64_t *out)
{
    uint64_t cur = seed;
    for (uint32_t i = 0; i < n; i++) {
        cur = derive_next_core(cur, faces[i], i);
        if (out) out[i] = cur;
    }
}

/* ── reconstruct_from_watcher: legacy adapter (minimal) ── */
static inline uint32_t path_reconstruct_from_watcher(
    ReconstructedPath       *p,
    const WatcherCtx        *w)
{
    if (!p || !w || w->bp_count == 0) return 0;
    path_init(p);
    
    uint64_t cur = w->blueprints[0].master_core;
    p->nodes[0].master_core = cur;
    p->nodes[0].generation  = 0;
    p->node_count = 1;

    for (uint32_t i = 1; i < w->bp_count && p->node_count < RECONSTRUCT_MAX_NODES; i++) {
        uint8_t face = w->blueprints[i].face_idx;
        cur = derive_next_core(cur, face, i);
        
        p->nodes[p->node_count].master_core = cur;
        p->nodes[p->node_count].face_idx    = face;
        p->nodes[p->node_count].generation  = (uint8_t)i;
        p->nodes[p->node_count].chunk_idx   = i * TE_CYCLE;
        p->node_count++;
    }
    
    p->complete = 1;
    return p->node_count;
}

#endif /* GEO_RECONSTRUCT_PATH_H */
