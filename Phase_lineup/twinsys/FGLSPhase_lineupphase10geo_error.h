#pragma once
#ifndef GEO_ERROR_H
#define GEO_ERROR_H

/*
 * geo_error.h — Phase 10 Error System
 *
 * L0: WHE-lite  — step invariant / structural correctness
 * L1: XOR checksum — cheap integrity per chunk
 * L2+: rewind/RS already in geo_rewind.h / geo_fec_rs.h
 *
 * Zero dependencies beyond stdint/stdbool/string.
 * No float. No heap. Hard-fail design.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ── Error codes (hard fail) ─────────────────────────── */
#define GEO_ERR_OK            0u
#define GEO_ERR_NULL_INPUT    1u   /* NULL pointer passed to gate */
#define GEO_ERR_SIZE_ZERO     2u   /* zero-length input */
#define GEO_ERR_SIZE_ALIGN    3u   /* size not multiple of chunk_sz */
#define GEO_ERR_CHECKSUM      4u   /* L1 XOR mismatch */
#define GEO_ERR_INVARIANT     5u   /* L0 WHE deviation detected */
#define GEO_ERR_SEG_OVERFLOW  6u   /* segment index out of range */

typedef uint8_t GeoErr;

/* ── L1: XOR checksum per 64B chunk ─────────────────── */
static inline uint32_t geo_checksum(const uint8_t *chunk, uint16_t sz)
{
    uint32_t acc = 0;
    for (uint16_t i = 0; i < sz; i++)
        acc ^= ((uint32_t)chunk[i] << ((i & 3u) * 8u));
    return acc;
}

static inline bool geo_checksum_verify(const uint8_t *chunk,
                                        uint16_t sz,
                                        uint32_t expected)
{
    return geo_checksum(chunk, sz) == expected;
}

/* ── L0: WHE-lite — step invariant tracker ──────────── */
#define GEO_WHE_PHI  UINT64_C(0x9E3779B97F4A7C15)
#define GEO_WHE_SUSPICIOUS_K  144u   /* 1 TE cycle */

typedef struct {
    uint64_t fp;            /* running fingerprint */
    uint32_t violations;    /* deviation count */
    uint32_t step_count;
    uint8_t  suspicious;    /* stayed deviated > K steps */
    uint8_t  tail_on;
    uint8_t  _pad[2];
} GeoWhe;

static inline void geo_whe_init(GeoWhe *w) {
    memset(w, 0, sizeof(*w));
}

/* call once per step — expected vs actual enc/route value */
static inline void geo_whe_step(GeoWhe   *w,
                                 uint64_t  expected,
                                 uint64_t  actual,
                                 uint32_t  step)
{
    if (expected != actual) {
        w->violations++;
        w->fp ^= (actual * GEO_WHE_PHI) ^ ((uint64_t)step << 17);
        if (!w->tail_on) w->tail_on = 1;
    } else {
        if (w->tail_on) w->tail_on = 0;
    }
    if (w->tail_on && (step % GEO_WHE_SUSPICIOUS_K == 0))
        w->suspicious = 1;
    w->fp ^= (expected * GEO_WHE_PHI) ^ step;
    w->step_count++;
}

/* final fingerprint — deterministic, replay-safe */
static inline uint64_t geo_whe_final(const GeoWhe *w) {
    return w->fp
         ^ ((uint64_t)w->violations << 32)
         ^ w->step_count;
}

static inline bool geo_whe_clean(const GeoWhe *w) {
    return w->violations == 0 && !w->suspicious;
}

/* ── Input gate — call before any pipeline entry ────── */
static inline GeoErr geo_input_gate(const void   *buf,
                                     uint32_t      size,
                                     uint32_t      chunk_sz)
{
    if (!buf)                        return GEO_ERR_NULL_INPUT;
    if (size == 0u)                  return GEO_ERR_SIZE_ZERO;
    if (chunk_sz && size % chunk_sz) return GEO_ERR_SIZE_ALIGN;
    return GEO_ERR_OK;
}

/* ── Segment bounds check ────────────────────────────── */
static inline GeoErr geo_seg_check(uint32_t seg_id, uint32_t seg_total) {
    if (seg_total == 0u || seg_id >= seg_total) return GEO_ERR_SEG_OVERFLOW;
    return GEO_ERR_OK;
}

/* ── Error string (debug) ────────────────────────────── */
static inline const char *geo_err_str(GeoErr e) {
    switch (e) {
        case GEO_ERR_OK:           return "OK";
        case GEO_ERR_NULL_INPUT:   return "NULL_INPUT";
        case GEO_ERR_SIZE_ZERO:    return "SIZE_ZERO";
        case GEO_ERR_SIZE_ALIGN:   return "SIZE_ALIGN";
        case GEO_ERR_CHECKSUM:     return "CHECKSUM_FAIL";
        case GEO_ERR_INVARIANT:    return "INVARIANT_FAIL";
        case GEO_ERR_SEG_OVERFLOW: return "SEG_OVERFLOW";
        default:                   return "UNKNOWN";
    }
}

#endif /* GEO_ERROR_H */
