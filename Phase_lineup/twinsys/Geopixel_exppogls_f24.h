#ifndef POGLS_F24_H
#define POGLS_F24_H

#include <stdint.h>
#include <math.h>

/*
 * POGLS F24 — 24-bit float [sign:1][exp:7 biased+64][mant:16]
 * precision ~0.002% (~4.8 decimal digits)
 * exp range: [-64, 63] → covers ~1e-19 to ~9e18
 */

static inline uint32_t f24_pack(float x) {
    if (x == 0.0f) return 0;

    uint32_t sign = (x < 0.0f) ? 1u : 0u;
    if (sign) x = -x;

    if (x < 1e-20f) return 0;  /* denormal → 0 */

    int exp;
    float m = frexpf(x, &exp);  /* m in [0.5, 1.0) */

    /* clamp exp to 7-bit signed range [-64, 63] */
    if (exp >  63) { exp =  63; m = 1.0f - 1.0f/65536.0f; }
    if (exp < -64) return 0;

    uint32_t mant = (uint32_t)((int32_t)(m * 65536.0f + 0.5f)) & 0xFFFF; /* rounded */
    uint32_t ebits = (uint32_t)(exp + 64) & 0x7F;  /* biased, unambiguous sign */

    return (sign << 23) | (ebits << 16) | mant;
}

static inline float f24_unpack(uint32_t raw) {
    if (raw == 0) return 0.0f;

    int sign  = (int)((raw >> 23) & 1u);
    int exp   = (int)((raw >> 16) & 0x7Fu) - 64;  /* remove bias */
    float m   = (float)(raw & 0xFFFFu) / 65536.0f;

    float x = ldexpf(m, exp);
    return sign ? -x : x;
}

#endif /* POGLS_F24_H */
