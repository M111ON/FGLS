#ifndef GEO_LITE_FIELD_H
#define GEO_LITE_FIELD_H

#include <stdint.h>

#define FULL_MASK 0xFFFFFFFFFFFFFFFFULL

// =========================
// 1. Wireless Hilbert Gate
// =========================
static inline uint8_t wh_pass(uint64_t core, uint64_t inv) {
    return ((core ^ inv) == FULL_MASK);
}

// =========================
// 2. Dodeca Field (Passive)
// =========================
// map → weight 1–8 (เร็ว, no memory)
static inline uint8_t dodeca_weight(uint64_t x) {
    return (x & 0x7) + 1;   // 1..8
}

// pair = mirror (ตรงข้าม)
static inline uint8_t dodeca_pair(uint8_t w) {
    return 9 - w;
}

// distance (สำคัญสุด)
static inline uint8_t dodeca_error(uint8_t a, uint8_t b) {
    uint8_t s = a + b;
    return (s > 9) ? (s - 9) : (9 - s);
}

// full score (block-level)
static inline uint32_t dodeca_score(uint64_t v) {
    uint8_t w0 = dodeca_weight(v >> 0);
    uint8_t w1 = dodeca_weight(v >> 8);
    uint8_t w2 = dodeca_weight(v >> 16);
    uint8_t w3 = dodeca_weight(v >> 24);

    uint32_t err = 0;
    err += dodeca_error(w0, dodeca_pair(w0));
    err += dodeca_error(w1, dodeca_pair(w1));
    err += dodeca_error(w2, dodeca_pair(w2));
    err += dodeca_error(w3, dodeca_pair(w3));

    return err; // 0 = perfect
}

// =========================
// 3. Geo Route (Zero-cost)
// =========================
typedef enum {
    ROUTE_DROP = 0,
    ROUTE_MAIN = 1,
    ROUTE_TEMP = 2
} route_t;

static inline route_t geo_route(uint64_t core,
                                 uint64_t inv,
                                 uint64_t addr) {
    if (!wh_pass(core, inv))
        return ROUTE_DROP;

    // LSB = path encoding
    return (addr & 1) ? ROUTE_MAIN : ROUTE_TEMP;
}

// =========================
// 4. Hybrid Decision (ของจริง)
// =========================
static inline route_t geo_decide(uint64_t core,
                                 uint64_t inv,
                                 uint64_t addr) {

    // L0: Fast drop
    if (!wh_pass(core, inv))
        return ROUTE_DROP;

    // L1: Dodeca field (passive drift measure)
    uint32_t err = dodeca_score(core);

    // ถ้า drift สูง → บังคับ TEMP (quarantine)
    if (err > 4)
        return ROUTE_TEMP;

    // L2: normal route
    return (addr & 1) ? ROUTE_MAIN : ROUTE_TEMP;
}

#endif