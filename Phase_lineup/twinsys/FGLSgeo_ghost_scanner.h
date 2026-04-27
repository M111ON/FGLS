/*
 * geo_ghost_scanner.h — Ghost Watcher × Scanner Wire  (S35)
 * ==========================================================
 * Role: สอง concern ใน header เดียว:
 *
 *   N1 — GhostScanCtx  : scan_cb glue
 *         scan_buf(buf, len, ghost_scan_cb, &gsc, cfg)
 *         → per ScanEntry → watcher_feed(core, spoke, slot_hot=1)
 *         → WatcherCtx accumulates blueprints
 *
 *   N2 — ghost_replay  : blueprint chain → chunk_idx stream
 *         ghost_replay(chain, n, cb, user)
 *         → per GhostRef → cb(chunk_idx, user)
 *         Callback style: no alloc, streaming-safe
 *
 * Design decisions (locked S35):
 *   slot_hot = 1 always (density tuning = future phase)
 *   spoke    = e->coord.face % 6  (ThetaCoord.face → cylinder spoke)
 *   chunk_idx in replay = blueprint index × TE_CYCLE  (boundary chunk)
 *
 * Full pipeline:
 *   scan_buf → ghost_scan_cb → watcher_feed → blueprints
 *                                              ↓
 *                               ghost_replay → cb(chunk_idx)
 *
 * Frozen rules:
 *   - No heap, no float, no global state
 *   - GhostScanCtx must outlive scan_buf call
 *   - ghost_replay is pure (read-only on chain)
 * ==========================================================
 */

#ifndef GEO_GHOST_SCANNER_H
#define GEO_GHOST_SCANNER_H

#include <stdint.h>
#include <stddef.h>
#include "core/pogls_scanner.h"      /* ScanEntry, scan_cb, scan_buf        */
#include "geo_ghost_watcher.h"  /* WatcherCtx, watcher_feed, GhostRef  */

/* ══════════════════════════════════════════════════════════════════
 * N1 — GhostScanCtx: scan_cb-compatible glue
 * ══════════════════════════════════════════════════════════════════ */

typedef struct {
    WatcherCtx *watcher;   /* caller-owned, must be pre-initialised   */
    uint32_t    bp_events; /* count of GHOST_BLUEPRINT events emitted  */
    uint32_t    _pad;
} GhostScanCtx;

/*
 * ghost_scan_cb — drop-in scan_cb for scan_buf()
 *
 * Maps ScanEntry fields to watcher_feed():
 *   core     = e->seed          (64-bit chunk fingerprint)
 *   spoke    = e->coord.face%6  (ThetaCoord → cylinder spoke 0-5)
 *   slot_hot = 1 (fixed, S35 lock — density tuning deferred)
 *
 * Safe to call with passthru chunks — watcher_feed handles them
 * identically (ghost doesn't care about compression status).
 */
static inline void ghost_scan_cb(const ScanEntry *e, void *user)
{
    GhostScanCtx *gsc = (GhostScanCtx *)user;
    if (!gsc || !gsc->watcher) return;

    uint64_t core     = e->seed;
    uint8_t  spoke    = (uint8_t)(e->coord.face % 6);
    uint8_t  slot_hot = 1u;  /* S35 lock: always hot */

    uint8_t result = watcher_feed(gsc->watcher, core, spoke, slot_hot);
    if (result == GHOST_BLUEPRINT) {
        gsc->bp_events++;
    }
}

/*
 * ghost_scan_init — zero GhostScanCtx and bind watcher
 *
 * watcher must already be initialised with watcher_init() before call.
 */
static inline void ghost_scan_init(GhostScanCtx *gsc, WatcherCtx *w)
{
    gsc->watcher   = w;
    gsc->bp_events = 0;
    gsc->_pad      = 0;
}

/* ══════════════════════════════════════════════════════════════════
 * N2 — ghost_replay: blueprint chain → chunk_idx stream
 * ══════════════════════════════════════════════════════════════════ */

/*
 * ghost_replay_cb — called once per blueprint in chain
 *
 * @chunk_idx : boundary chunk index where this blueprint was sealed
 *              = (blueprint_position + 1) × TE_CYCLE - 1
 *              e.g. blueprint[0] → chunk 143, blueprint[1] → chunk 287
 * @ref       : the GhostRef itself (master_core + face_idx)
 * @user      : opaque caller pointer
 */
typedef void (*ghost_replay_cb)(uint32_t         chunk_idx,
                                const GhostRef  *ref,
                                void            *user);

/*
 * ghost_replay — replay blueprint chain via callback
 *
 * @chain : pointer to GhostRef array (from WatcherCtx.blueprints)
 * @n     : number of valid blueprints (WatcherCtx.bp_count)
 * @cb    : called once per blueprint (never NULL)
 * @user  : opaque pointer forwarded to cb
 *
 * Chunk index derivation:
 *   blueprint[i] was sealed when chunk_count crossed (i+1)*TE_CYCLE
 *   → boundary chunk = (i+1)*TE_CYCLE - 1  (0-based)
 *
 * Pure function: no state mutation, chain is read-only.
 * Safe to call multiple times on same chain.
 */
static inline void ghost_replay(const GhostRef  *chain,
                                 uint32_t         n,
                                 ghost_replay_cb  cb,
                                 void            *user)
{
    if (!chain || !cb || n == 0) return;

    for (uint32_t i = 0; i < n; i++) {
        /* boundary chunk index where blueprint[i] was sealed */
        uint32_t chunk_idx = (i + 1u) * (uint32_t)TE_CYCLE - 1u;
        cb(chunk_idx, &chain[i], user);
    }
}

/*
 * ghost_replay_watcher — convenience: replay directly from WatcherCtx
 *
 * Equivalent to ghost_replay(w->blueprints, w->bp_count, cb, user).
 */
static inline void ghost_replay_watcher(const WatcherCtx *w,
                                         ghost_replay_cb   cb,
                                         void             *user)
{
    ghost_replay(w->blueprints, w->bp_count, cb, user);
}

/* ══════════════════════════════════════════════════════════════════
 * Convenience: full pipeline helper
 * ══════════════════════════════════════════════════════════════════ */

/*
 * ghost_scan_and_replay — run full pipeline in one call
 *
 * 1. scan_buf(buf, len) → ghost_scan_cb → WatcherCtx fills blueprints
 * 2. ghost_replay_watcher → cb(chunk_idx, ref, user) per blueprint
 *
 * @genesis : initial GeoSeed for WatcherCtx
 * @cfg     : ScanConfig (NULL → defaults)
 * @cb      : replay callback (NULL = scan only, no replay)
 *
 * Returns: number of chunks scanned (from scan_buf).
 *
 * WatcherCtx w is stack-allocated inside; blueprints live only
 * for duration of this call — cb must copy ref if needed beyond.
 */
static inline uint32_t ghost_scan_and_replay(
    const uint8_t  *buf,
    size_t          len,
    GeoSeed         genesis,
    const ScanConfig *cfg,
    ghost_replay_cb  cb,
    void            *user)
{
    WatcherCtx   w;
    GhostScanCtx gsc;

    watcher_init(&w, genesis);
    ghost_scan_init(&gsc, &w);

    uint32_t chunks = scan_buf(buf, len, ghost_scan_cb, &gsc, cfg);

    if (cb) {
        ghost_replay_watcher(&w, cb, user);
    }

    return chunks;
}

#endif /* GEO_GHOST_SCANNER_H */
