/*
 * geo_gf256.h — GF(2^8) arithmetic
 * ══════════════════════════════════
 * Primitive poly: 0x11D (x^8+x^4+x^3+x^2+1) — standard RS
 * exp/log tables baked at compile-time via init call (once, static).
 *
 * API:
 *   gf256_init()          — init tables (call once)
 *   gf256_mul(a,b)        — a*b in GF(256)
 *   gf256_inv(a)          — a^-1 in GF(256) (a!=0)
 *   gf256_pow(a,n)        — a^n in GF(256)
 *   gf256_div(a,b)        — a/b = a*inv(b)
 */
#ifndef GEO_GF256_H
#define GEO_GF256_H

#include <stdint.h>
#include <string.h>

/* ── tables (255 non-zero elements, index 255 = wraparound) ── */
static uint8_t _GF_EXP[512]; /* exp[i] = g^i mod p, extended for mul wrap */
static uint8_t _GF_LOG[256]; /* log[a] = i where g^i = a */
static int     _gf256_ready = 0;

static inline void gf256_init(void) {
    if (_gf256_ready) return;
    uint16_t x = 1u;
    for (int i = 0; i < 255; i++) {
        _GF_EXP[i] = (uint8_t)x;
        _GF_LOG[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100u) x ^= 0x11Du;
    }
    /* extend for wrap-around in mul */
    for (int i = 255; i < 512; i++)
        _GF_EXP[i] = _GF_EXP[i - 255];
    _GF_LOG[0]   = 0; /* undefined, guard value */
    _gf256_ready = 1;
}

static inline uint8_t gf256_mul(uint8_t a, uint8_t b) {
    if (!a || !b) return 0u;
    return _GF_EXP[(uint16_t)_GF_LOG[a] + _GF_LOG[b]];
}

static inline uint8_t gf256_inv(uint8_t a) {
    /* a^-1 = g^(255 - log(a)) */
    return _GF_EXP[255u - _GF_LOG[a]];
}

static inline uint8_t gf256_pow(uint8_t a, uint8_t n) {
    if (n == 0u) return 1u;
    if (!a)      return 0u;
    return _GF_EXP[((uint16_t)_GF_LOG[a] * n) % 255u];
}

static inline uint8_t gf256_div(uint8_t a, uint8_t b) {
    if (!a) return 0u;
    return gf256_mul(a, gf256_inv(b));
}

static inline uint8_t gf256_add(uint8_t a, uint8_t b) {
    return a ^ b; /* GF(2^8) addition = XOR */
}

#endif /* GEO_GF256_H */
