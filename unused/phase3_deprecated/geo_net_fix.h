/*
 * geo_net_fix.h — N3 & N4 Fixes
 * ═══════════════════════════════════════════════════════════════════
 *
 * N3: dodeca_score always 0 - เพิ่ม score tracking ใน geo_net
 * N4: geonet_map_fast wrong Barrett constant - ใช้ constant ที่ถูกต้อง
 */

#ifndef GEO_NET_FIX_H
#define GEO_NET_FIX_H

#include <stdint.h>
#include "geo_cylinder.h"
#include "geo_thirdeye.h"

/* ═══════════════════════════════════════════════════════════════════
 * N4 FIX: Barrett constant สำหรับ mod6
 * ═══════════════════════════════════════════════════════════════════
 *
 * Original: 10923U (2^16 / 6 = 10922.67)
 * Issue: ถ้าใช้สำหรับ slot/face conversion อาจผิด
 * Fix: ใช้ 2731 สำหรับ (2^14 / 6) ที่ละเอียดกว่า
 * 
 * Barrett constant formula: floor(2^k / n)
 *   k=16: 65536/6 = 10922.67 → 10923
 *   k=14: 16384/6 = 2730.67 → 2731
 */
#define BARRETT_MOD6_K16  10923U   /* original - สำหรับ general use */
#define BARRETT_MOD6_K14  2731U    /* fix: สำหรับ slot→spoke conversion */

/* N4: Fixed fast_mod6 - ใช้ k=14 สำหรับ precise conversion */
static inline uint8_t _gn_mod6_fixed(uint32_t n) {
    uint32_t q = (n * BARRETT_MOD6_K14) >> 14;
    return (uint8_t)(n - q * 6U);
}

/* N4: Alternative - k=16 (original, faster but less precise) */
static inline uint8_t _gn_mod6_original(uint32_t n) {
    uint32_t q = (n * BARRETT_MOD6_K16) >> 16;
    return (uint8_t)(n - q * 6U);
}

/* ═══════════════════════════════════════════════════════════════════
 * N3 FIX: dodeca_score tracking
 * ═══════════════════════════════════════════════════════════════════
 *
 * Issue: dodeca_score always 0
 * Root cause: ไม่มี score accumulation ใน routing path
 * Fix: เพิ่ม score tracking ตาม valid dodeca transforms
 */

#define DODECA_PATHS 12    /* 12 dodeca faces */
#define DODECA_WINDOW_LO 96   /* min valid score */
#define DODECA_WINDOW_HI 128  /* max valid score */

typedef struct {
    uint32_t dodeca_score;     /* accumulated score */
    uint32_t valid_paths;      /* count of valid dodeca paths */
    uint8_t  last_verdict;     /* 1=pass, 0=fail */
} DodecaStats;

/* N3: Calculate dodeca score from 12 transforms */
static inline uint32_t calc_dodeca_score(const uint64_t *cores, uint32_t n) {
    if (!cores || n == 0) return 0;
    
    uint32_t score = 0;
    for (uint32_t i = 0; i < n && i < DODECA_PATHS; i++) {
        /* Score = popcount of core (higher entropy = better) */
        uint32_t pc = __builtin_popcountll(cores[i]);
        /* Normalize: 32-bit cores → 0-64 → 0-8 score */
        score += (pc / 8);
    }
    return score;
}

/* N3: Verdict based on score window */
static inline uint8_t dodeca_verdict(uint32_t score) {
    return (score >= DODECA_WINDOW_LO && score <= DODECA_WINDOW_HI) ? 1 : 0;
}

/* N3: Update dodeca stats */
static inline void dodeca_update(DodecaStats *ds, const uint64_t *cores, uint32_t n) {
    ds->dodeca_score = calc_dodeca_score(cores, n);
    ds->valid_paths = (n < DODECA_PATHS) ? n : DODECA_PATHS;
    ds->last_verdict = dodeca_verdict(ds->dodeca_score);
}

/* N3: Reset stats */
static inline void dodeca_reset(DodecaStats *ds) {
    ds->dodeca_score = 0;
    ds->valid_paths = 0;
    ds->last_verdict = 0;
}

#endif /* GEO_NET_FIX_H */