/*
 * lc_hdr.h — LetterCube Node Header v2
 * ═══════════════════════════════════════════════════════════════
 *
 * 32-bit packed header สำหรับ LC node ทุกตัวในระบบ
 *
 * Layout (32 bits):
 *
 *  [31]     sign/polarity  S   : 1 bit  — ขั้ว +/- (0=pos 1=neg)
 *  [30:24]  magnitude      V   : 7 bits — 0=ghost, 1-127=active
 *  [23:21]  red            R   : 3 bits — RGB9 (8 levels/ch)
 *  [20:18]  green          G   : 3 bits
 *  [17:15]  blue           B   : 3 bits
 *  [14:13]  level          L   : 2 bits — L0/L1/L2/L3 (×8 scale)
 *  [12:4]   angular        A   : 9 bits — 0-511 → map to 0-359°
 *  [3:2]    face           F   : 2 bits — Rubik face pair (0-3)
 *  [1:0]    reserved       _   : 2 bits — future / palette ext
 *
 * Total: 32 bits = 1 uint32_t per node header ✅
 *
 * Ghost rule: V==0 → ghost regardless of S,R,G,B
 * Complement: R1+R2==7, G1+G2==7, B1+B2==7, S1!=S2
 * Warp gate:  complement + both active (V>0)
 * Angular:    0-511 maps to 0-360° (512 steps, 0.703°/step)
 *             chiral pair: A_pair = (A + 256) % 512  (180° flip)
 *             TRing full cycle = 720 → 2 × angular cycle ✅
 *
 * Level scale (LC_LEVEL):
 *   L0 = 3,456   nodes  (54 × 64)
 *   L1 = 27,648  nodes  (L0 × 8)  ← EHCP space
 *   L2 = 221,184 nodes  (L1 × 8)
 *   L3 = 1,769,472 nodes (L2 × 8)
 *
 * Palette = bitboard mask — global module, called by LC
 * Color gate sits at LC level, not Rubik level
 *
 * No malloc. No float. No heap.
 * ═══════════════════════════════════════════════════════════════
 */

#ifndef LC_HDR_H
#define LC_HDR_H

#include <stdint.h>
#include <stdio.h>

/* ══════════════════════════════════════════════════════
   BIT LAYOUT CONSTANTS
   ══════════════════════════════════════════════════════ */
#define LCH_S_SHIFT   31u
#define LCH_V_SHIFT   24u
#define LCH_R_SHIFT   21u
#define LCH_G_SHIFT   18u
#define LCH_B_SHIFT   15u
#define LCH_L_SHIFT   13u
#define LCH_A_SHIFT    4u
#define LCH_F_SHIFT    2u

#define LCH_S_MASK   (0x1u  << LCH_S_SHIFT)   /* 1 bit  */
#define LCH_V_MASK   (0x7Fu << LCH_V_SHIFT)   /* 7 bits */
#define LCH_R_MASK   (0x7u  << LCH_R_SHIFT)   /* 3 bits */
#define LCH_G_MASK   (0x7u  << LCH_G_SHIFT)   /* 3 bits */
#define LCH_B_MASK   (0x7u  << LCH_B_SHIFT)   /* 3 bits */
#define LCH_L_MASK   (0x3u  << LCH_L_SHIFT)   /* 2 bits */
#define LCH_A_MASK   (0x1FFu << LCH_A_SHIFT)  /* 9 bits */
#define LCH_F_MASK   (0x3u  << LCH_F_SHIFT)   /* 2 bits */

/* ══════════════════════════════════════════════════════
   LEVEL CONSTANTS
   ══════════════════════════════════════════════════════ */
#define LC_LEVEL_0   0u   /*   3,456 nodes */
#define LC_LEVEL_1   1u   /*  27,648 nodes — EHCP */
#define LC_LEVEL_2   2u   /* 221,184 nodes */
#define LC_LEVEL_3   3u   /* 1,769,472 nodes */

#define LC_L0_NODES  3456u
#define LC_L1_NODES  27648u
#define LC_L2_NODES  221184u
#define LC_L3_NODES  1769472u

/* ══════════════════════════════════════════════════════
   ANGULAR CONSTANTS
   ══════════════════════════════════════════════════════ */
#define LCH_ANG_STEPS   512u    /* 0-511 → 0-360° */
#define LCH_ANG_HALF    256u    /* 180° flip = chiral pair */
#define LCH_TRING_CYCLE 720u    /* 2 × angular cycle ✅ */

/* ══════════════════════════════════════════════════════
   PACK / UNPACK
   ══════════════════════════════════════════════════════ */
typedef uint32_t LCHdr;

static inline LCHdr lch_pack(uint8_t  sign,   /* 0/1        */
                               uint8_t  mag,    /* 0-127      */
                               uint8_t  r,      /* 0-7        */
                               uint8_t  g,      /* 0-7        */
                               uint8_t  b,      /* 0-7        */
                               uint8_t  level,  /* 0-3        */
                               uint16_t angle,  /* 0-511      */
                               uint8_t  face)   /* 0-3        */
{
    return ((uint32_t)(sign  & 0x1u)   << LCH_S_SHIFT) |
           ((uint32_t)(mag   & 0x7Fu)  << LCH_V_SHIFT) |
           ((uint32_t)(r     & 0x7u)   << LCH_R_SHIFT) |
           ((uint32_t)(g     & 0x7u)   << LCH_G_SHIFT) |
           ((uint32_t)(b     & 0x7u)   << LCH_B_SHIFT) |
           ((uint32_t)(level & 0x3u)   << LCH_L_SHIFT) |
           ((uint32_t)(angle & 0x1FFu) << LCH_A_SHIFT) |
           ((uint32_t)(face  & 0x3u)   << LCH_F_SHIFT);
}

/* ── accessors ── */
static inline uint8_t  lch_sign(LCHdr h)  { return (uint8_t)((h >> LCH_S_SHIFT) & 0x1u);   }
static inline uint8_t  lch_mag(LCHdr h)   { return (uint8_t)((h >> LCH_V_SHIFT) & 0x7Fu);  }
static inline uint8_t  lch_r(LCHdr h)     { return (uint8_t)((h >> LCH_R_SHIFT) & 0x7u);   }
static inline uint8_t  lch_g(LCHdr h)     { return (uint8_t)((h >> LCH_G_SHIFT) & 0x7u);   }
static inline uint8_t  lch_b(LCHdr h)     { return (uint8_t)((h >> LCH_B_SHIFT) & 0x7u);   }
static inline uint8_t  lch_level(LCHdr h) { return (uint8_t)((h >> LCH_L_SHIFT) & 0x3u);   }
static inline uint16_t lch_angle(LCHdr h) { return (uint16_t)((h >> LCH_A_SHIFT) & 0x1FFu);}
static inline uint8_t  lch_face(LCHdr h)  { return (uint8_t)((h >> LCH_F_SHIFT) & 0x3u);   }

/* ── state checks ── */
static inline int lch_is_ghost(LCHdr h)  { return lch_mag(h) == 0; }
static inline int lch_is_active(LCHdr h) { return lch_mag(h)  > 0; }
static inline int lch_is_pos(LCHdr h)    { return lch_is_active(h) && lch_sign(h) == 0; }
static inline int lch_is_neg(LCHdr h)    { return lch_is_active(h) && lch_sign(h) == 1; }

/* ── ghost: zero magnitude, keep RGB+sign as audit trail ── */
static inline LCHdr lch_ghost(LCHdr h) {
    return h & ~LCH_V_MASK;   /* V→0, everything else preserved */
}

/* ── flip polarity (toggle sign bit) ── */
static inline LCHdr lch_flip(LCHdr h) {
    if (lch_is_ghost(h)) return h;   /* ghost ไม่ flip */
    return h ^ LCH_S_MASK;
}

/* ── complement check: RGB7 complement + opposite sign + both active ── */
static inline int lch_is_complement(LCHdr a, LCHdr b) {
    if (lch_is_ghost(a) || lch_is_ghost(b)) return 0;
    return (lch_r(a) + lch_r(b) == 7u) &&
           (lch_g(a) + lch_g(b) == 7u) &&
           (lch_b(a) + lch_b(b) == 7u) &&
           (lch_sign(a) != lch_sign(b));
}

/* ── angular: chiral pair (180° flip) ── */
static inline uint16_t lch_chiral(LCHdr h) {
    return (uint16_t)((lch_angle(h) + LCH_ANG_HALF) % LCH_ANG_STEPS);
}

/* ── angular: TRing position (angle → walk index 0-719) ── */
static inline uint16_t lch_tring_pos(LCHdr h) {
    return (uint16_t)((uint32_t)lch_angle(h) * LCH_TRING_CYCLE / LCH_ANG_STEPS);
}

/* ══════════════════════════════════════════════════════
   PALETTE — bitboard mask (global module)
   64-bit per face at L0, scales ×8 per level
   ══════════════════════════════════════════════════════ */
typedef struct {
    uint64_t mask[4];    /* 4 faces × 64 bits = L0 palette */
    uint8_t  level;      /* LC_LEVEL_0..3 */
} LCPalette;

/* filter: node passes if its face bit is set in palette mask */
static inline int lch_palette_pass(const LCPalette *p, LCHdr h) {
    uint8_t f = lch_face(h);
    if (f >= 4u) return 0;
    /* at L0: use angle as bit index within 64-bit face mask */
    uint64_t bit = 1ull << (lch_angle(h) & 63u);
    return !!(p->mask[f] & bit);
}

/* set face bit */
static inline void lch_palette_set(LCPalette *p, uint8_t face, uint16_t angle) {
    if (face >= 4u) return;
    p->mask[face] |= 1ull << (angle & 63u);
}

/* clear = ground lane */
static inline void lch_palette_clear(LCPalette *p, uint8_t face, uint16_t angle) {
    if (face >= 4u) return;
    p->mask[face] &= ~(1ull << (angle & 63u));
}

/* ══════════════════════════════════════════════════════
   GATE RESULT
   ══════════════════════════════════════════════════════ */
typedef enum {
    LCG_WARP,
    LCG_COLLISION,
    LCG_FILTER_BLOCK,
    LCG_GROUND_ABSORB,
} LCGate;

static inline LCGate lch_gate(LCHdr a, LCHdr b,
                                const LCPalette *pal_a,
                                const LCPalette *pal_b) {
    /* ghost check first */
    if (lch_is_ghost(a) || lch_is_ghost(b)) return LCG_GROUND_ABSORB;
    /* palette filter */
    if (pal_a && !lch_palette_pass(pal_a, a)) return LCG_GROUND_ABSORB;
    if (pal_b && !lch_palette_pass(pal_b, b)) return LCG_GROUND_ABSORB;
    /* complement = warp */
    if (lch_is_complement(a, b)) return LCG_WARP;
    /* same RGB + same sign = collision */
    if (lch_r(a)==lch_r(b) && lch_g(a)==lch_g(b) &&
        lch_b(a)==lch_b(b) && lch_sign(a)==lch_sign(b))
        return LCG_COLLISION;
    return LCG_FILTER_BLOCK;
}

/* ── debug print ── */
static inline void lch_print(const char *label, LCHdr h) {
    printf("  %-8s hdr=0x%08X  %s  sign=%c  mag=%3d"
           "  rgb=(%d,%d,%d)  L%d  ang=%3d  face=%d\n",
           label, h,
           lch_is_ghost(h) ? "GHOST " : (lch_is_pos(h) ? "POS   " : "NEG   "),
           lch_sign(h) ? '-' : '+',
           lch_mag(h),
           lch_r(h), lch_g(h), lch_b(h),
           lch_level(h),
           lch_angle(h),
           lch_face(h));
}

#endif /* LC_HDR_H */

/* ══════════════════════════════════════════════════════
   SELF-TEST
   gcc -O2 -o lc_hdr_test lc_hdr.h -x c -DTEST_LC_HDR
   ══════════════════════════════════════════════════════ */
#ifdef TEST_LC_HDR
#include <assert.h>

int main(void) {
    int pass=0, fail=0;
#define CHK(cond,msg) do{ if(cond){printf("  ✓ %s\n",msg);pass++;} \
                          else{printf("  ✗ FAIL: %s\n",msg);fail++;}  }while(0)

    printf("╔══════════════════════════════════════════════╗\n");
    printf("║  lc_hdr.h v2 — Self Test                    ║\n");
    printf("╚══════════════════════════════════════════════╝\n\n");

    /* pack/unpack */
    printf("▶ Pack/Unpack roundtrip\n");
    LCHdr h = lch_pack(0, 100, 5, 3, 1, LC_LEVEL_1, 180, 2);
    lch_print("h", h);
    CHK(lch_sign(h)==0,   "sign=0");
    CHK(lch_mag(h)==100,  "mag=100");
    CHK(lch_r(h)==5,      "r=5");
    CHK(lch_g(h)==3,      "g=3");
    CHK(lch_b(h)==1,      "b=1");
    CHK(lch_level(h)==1,  "level=L1");
    CHK(lch_angle(h)==180,"angle=180");
    CHK(lch_face(h)==2,   "face=2");

    /* ghost */
    printf("\n▶ Ghost\n");
    LCHdr g = lch_ghost(h);
    lch_print("ghost", g);
    CHK(lch_is_ghost(g),        "mag=0 = ghost");
    CHK(lch_r(g)==5,            "RGB preserved as trail");
    CHK(lch_sign(g)==0,         "sign preserved as trail");

    /* flip */
    printf("\n▶ Flip polarity\n");
    LCHdr f = lch_flip(h);
    lch_print("flip", f);
    CHK(lch_sign(f)==1,         "sign flipped to 1");
    CHK(lch_mag(f)==100,        "mag unchanged");
    CHK(lch_flip(f)==h,         "flip×2 = identity");
    LCHdr fg = lch_flip(g);
    CHK(fg==g,                  "ghost flip = no-op");

    /* complement pair */
    printf("\n▶ Complement pair\n");
    LCHdr a = lch_pack(0, 64, 5, 3, 1, LC_LEVEL_1, 0,   0);
    LCHdr b = lch_pack(1, 64, 2, 4, 6, LC_LEVEL_1, 256, 0);
    lch_print("A(+)", a);
    lch_print("B(-)", b);
    CHK(lch_is_complement(a,b), "A↔B complement (5+2=7,3+4=7,1+6=7,diff sign)");
    CHK(lch_gate(a,b,NULL,NULL)==LCG_WARP, "gate=WARP");

    /* same color same sign = collision */
    LCHdr c1 = lch_pack(0, 50, 3, 3, 3, LC_LEVEL_1, 0, 0);
    LCHdr c2 = lch_pack(0, 50, 3, 3, 3, LC_LEVEL_1, 0, 0);
    CHK(lch_gate(c1,c2,NULL,NULL)==LCG_COLLISION, "same RGB+sign = COLLISION");

    /* ghost gate */
    CHK(lch_gate(g,b,NULL,NULL)==LCG_GROUND_ABSORB, "ghost = GROUND_ABSORB");

    /* angular chiral */
    printf("\n▶ Angular chiral pair\n");
    CHK(lch_chiral(a)==256, "chiral(0) = 256 (180°)");
    CHK(lch_chiral(b)==0,   "chiral(256) = 0 (back)");
    uint16_t tp = lch_tring_pos(a);
    printf("  tring_pos(angle=0) = %u  (expect 0)\n", tp);
    LCHdr ang180 = lch_pack(0,64,5,3,1,0,180,0);
    uint16_t tp2 = lch_tring_pos(ang180);
    printf("  tring_pos(angle=180) = %u  (expect ~253)\n", tp2);
    CHK(tp==0, "tring_pos(0)=0");
    CHK(tp2 > 200 && tp2 < 280, "tring_pos(180) ~253 (half cycle)");

    /* palette */
    printf("\n▶ Palette bitboard\n");
    LCPalette pal = {0};
    lch_palette_set(&pal, 0, 0);   /* face0, angle=0 → bit0 */
    CHK( lch_palette_pass(&pal, a), "palette pass (bit set)");
    LCHdr b2 = lch_pack(1,64,2,4,6,LC_LEVEL_1,100,0);
    CHK(!lch_palette_pass(&pal, b2), "palette block (bit not set, angle=100)");
    lch_palette_clear(&pal, 0, 0);
    CHK(!lch_palette_pass(&pal, a), "palette clear → ground lane");

    /* sizeof */
    printf("\n▶ sizeof\n");
    printf("  sizeof(LCHdr)    = %zu  (expect 4)\n", sizeof(LCHdr));
    printf("  sizeof(LCPalette)= %zu\n", sizeof(LCPalette));
    CHK(sizeof(LCHdr)==4,      "LCHdr = 4 bytes");
    CHK(sizeof(LCPalette)==40, "LCPalette = 40 bytes");

    printf("\n══════════════════════════════════════════════\n");
    printf("  PASS: %d   FAIL: %d\n", pass, fail);
    if (!fail) printf("✅ All PASS\n");
    else       printf("❌ %d FAIL\n", fail);
    return fail ? 1 : 0;
}
#endif
