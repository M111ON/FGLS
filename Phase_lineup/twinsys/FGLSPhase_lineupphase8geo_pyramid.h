/*
 * geo_pyramid.h — Phase 8: Residual Layer Pyramid
 * ═══════════════════════════════════════════════
 *
 * Exposes the 5-level structure latent inside TRing's 720-cycle:
 *
 *   720 = 5 × 144   (144 = F(12) = 1 fibo clock = 1 GCFS dispatch)
 *
 *   phase = pos / 144   → 0..4  (pyramid level L0–L4)
 *   slot  = pos % 144   → 0..143 (position within phase)
 *
 * Nothing new is stored. pyramid functions are pure derivations
 * from walk position — zero added state, zero heap, zero float.
 *
 * L0 = pos   0– 143  (first  fibo cycle)
 * L1 = pos 144– 287
 * L2 = pos 288– 431
 * L3 = pos 432– 575
 * L4 = pos 576– 719  (last   fibo cycle)
 *
 * Residual = slot within a phase that has not been filled.
 * A phase is "complete" when all 144 slots are present.
 * Cross-phase boundary = pyramid level transition.
 *
 * No malloc. No float. No heap.
 * ═══════════════════════════════════════════════
 */

#ifndef GEO_PYRAMID_H
#define GEO_PYRAMID_H

#include <stdint.h>
#include "geo_temporal_ring.h"

/* ── Constants ────────────────────────────────────────────────── */
#define GEO_PYR_DEPTH        5u     /* 720 / 144                  */
#define GEO_PYR_PHASE_LEN  144u     /* F(12) = 1 fibo clock       */
#define GEO_PYR_TOTAL      720u     /* must equal TEMPORAL_WALK_LEN */

typedef char _pyr_depth_assert [(GEO_PYR_DEPTH * GEO_PYR_PHASE_LEN == GEO_PYR_TOTAL) ? 1:-1];
typedef char _pyr_total_assert [(GEO_PYR_TOTAL == TEMPORAL_WALK_LEN) ? 1:-1];

/* ── PyramidPos: decomposed walk position ─────────────────────── */
typedef struct {
    uint8_t  level;    /* 0..4  which fibo phase (L0–L4)          */
    uint8_t  slot;     /* 0..143 position within phase             */
    uint16_t pos;      /* original walk position 0..719            */
} PyramidPos;

/* ── Phase state summary ──────────────────────────────────────── */
typedef struct {
    uint8_t  level;         /* 0..4                               */
    uint8_t  present;       /* slots filled in this phase         */
    uint8_t  residual;      /* 144 - present (missing slots)      */
    uint8_t  complete;      /* 1 = all 144 present                */
    uint16_t first_gap;     /* walk pos of first missing slot,
                               0xFFFF = none                      */
} PyramidPhase;

/* ── pyr_decompose: walk pos → level + slot ─────────────────────
 * O(1) — two integer ops                                         */
static inline PyramidPos pyr_decompose(uint16_t pos)
{
    PyramidPos p;
    p.pos   = pos;
    p.level = (uint8_t)(pos / GEO_PYR_PHASE_LEN);
    p.slot  = (uint8_t)(pos % GEO_PYR_PHASE_LEN);
    return p;
}

/* ── pyr_compose: level + slot → walk pos ───────────────────────
 * O(1)                                                           */
static inline uint16_t pyr_compose(uint8_t level, uint8_t slot)
{
    return (uint16_t)(level * GEO_PYR_PHASE_LEN + slot);
}

/* ── pyr_head_level: current pyramid level from ring head ───────
 * O(1)                                                           */
static inline uint8_t pyr_head_level(const TRingCtx *r)
{
    return (uint8_t)(r->head / GEO_PYR_PHASE_LEN);
}

/* ── pyr_phase_start / end: walk pos bounds for a level ─────────
 * O(1)                                                           */
static inline uint16_t pyr_phase_start(uint8_t level)
{
    return (uint16_t)(level * GEO_PYR_PHASE_LEN);
}
static inline uint16_t pyr_phase_end(uint8_t level)
{
    return (uint16_t)(level * GEO_PYR_PHASE_LEN + GEO_PYR_PHASE_LEN - 1u);
}

/* ── pyr_scan_phase: scan one level for residuals ───────────────
 * O(144) — scans exactly one phase worth of slots
 * Fills PyramidPhase summary.                                    */
static inline PyramidPhase pyr_scan_phase(const TRingCtx *r, uint8_t level)
{
    PyramidPhase ph;
    ph.level     = level;
    ph.present   = 0u;
    ph.first_gap = 0xFFFFu;

    uint16_t base = pyr_phase_start(level);
    for (uint8_t s = 0u; s < GEO_PYR_PHASE_LEN; s++) {
        uint16_t pos = (uint16_t)(base + s);
        if (r->slots[pos].present) {
            ph.present++;
        } else if (ph.first_gap == 0xFFFFu) {
            ph.first_gap = pos;
        }
    }
    ph.residual  = (uint8_t)(GEO_PYR_PHASE_LEN - ph.present);
    ph.complete  = (ph.residual == 0u) ? 1u : 0u;
    return ph;
}

/* ── pyr_scan_all: scan all 5 levels ────────────────────────────
 * O(720) — fills out[5]                                          */
static inline void pyr_scan_all(const TRingCtx *r, PyramidPhase out[GEO_PYR_DEPTH])
{
    for (uint8_t l = 0u; l < GEO_PYR_DEPTH; l++)
        out[l] = pyr_scan_phase(r, l);
}

/* ── pyr_is_level_transition: did head just cross a phase boundary
 * Call after tring_tick / tring_verify_next.
 * Returns 1 if head is at slot 0 of a new level (just crossed),
 *         0 otherwise.
 * Use to trigger per-level flush / GCFS dispatch.               */
static inline int pyr_is_level_transition(const TRingCtx *r)
{
    return (r->head % GEO_PYR_PHASE_LEN == 0u) ? 1 : 0;
}

/* ── pyr_residual_count: total missing across all phases ─────────
 * O(720)                                                         */
static inline uint16_t pyr_residual_count(const TRingCtx *r)
{
    uint16_t cnt = 0u;
    for (uint16_t i = 0u; i < GEO_PYR_TOTAL; i++)
        if (!r->slots[i].present) cnt++;
    return cnt;
}

/* ── pyr_complete_levels: bitmask of complete levels ────────────
 * bit i set → level i has all 144 slots present
 * O(720)                                                         */
static inline uint8_t pyr_complete_levels(const TRingCtx *r)
{
    uint8_t mask = 0u;
    for (uint8_t l = 0u; l < GEO_PYR_DEPTH; l++) {
        PyramidPhase ph = pyr_scan_phase(r, l);
        if (ph.complete) mask |= (uint8_t)(1u << l);
    }
    return mask;
}

#endif /* GEO_PYRAMID_H */
