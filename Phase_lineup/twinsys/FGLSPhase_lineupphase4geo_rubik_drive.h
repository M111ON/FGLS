/*
 * geo_rubik_drive.h — Rubik Cube Drive Engine (base2 × base3)
 * ════════════════════════════════════════════════════════════
 *
 * Entire Rubik state driven by 2 numbers only: 2 and 3
 *
 *   base2 = Hilbert addressing  (2^n family)
 *   base3 = routing decision    (0=pair / 1=branch / 2=skip)
 *
 * Rubik cube geometry:
 *   6 faces × 9 cells = 54 cells  (sacred: 54 = 2 × 3³ ✅)
 *   3 axes  × 3 layers = 9 moves  (base3 per axis)
 *   face encoding: 2 bits per cell → 54 × 2 = 108 bits → 2 × uint64
 *
 * Bundle link (cpu_derive_bundle):
 *   gen2 ^ gen3 → mix → rotl(12/18/24) → 9 words → 9 hilbert paths
 *   = ApexWire upstream ✅
 *
 * Drive rule:
 *   state = (state × 2) % 3   → cycles: 1→2→1  (period 2)
 *   state = (state × 3) % 2   → always 0        (collapse)
 *   useful: (addr × 2) % 3    → route ∈ {0,1,2} deterministic
 *           (addr × 3) % 2    → parity ∈ {0,1}
 *
 * No malloc, no float, no heap.
 * ════════════════════════════════════════════════════════════
 */

#ifndef GEO_RUBIK_DRIVE_H
#define GEO_RUBIK_DRIVE_H

#include <stdint.h>
#include "geo_primitives.h"
#include "pogls_fibo_addr.h"

/* ── Constants ───────────────────────────────────────────────── */
#define RUB_FACES        6u    /* cube faces                       */
#define RUB_CELLS_FACE   9u    /* cells per face (3×3)             */
#define RUB_CELLS       54u    /* total = 6×9 = sacred 54 ✅       */
#define RUB_AXES         3u    /* X Y Z                            */
#define RUB_LAYERS       3u    /* per axis (base3)                 */
#define RUB_BITS_CELL    2u    /* 2 bits per cell state            */
#define RUB_WORDS        2u    /* 54×2=108 bits → 2×uint64 (128b) */

/* ── RubikState: 2 × uint64 = 128 bits = full 54-cell state ─── */
typedef struct {
    uint64_t w[RUB_WORDS];    /* packed cell states (2bit each)   */
    uint64_t seed;             /* origin seed                      */
    uint32_t move_count;       /* moves applied                    */
    uint8_t  axis;             /* last move axis 0=X 1=Y 2=Z      */
    uint8_t  layer;            /* last move layer 0..2 (base3)     */
    uint8_t  _pad[2];
} RubikState;                  /* 32B ✅                           */

/* ── Base2/3 drive primitives ────────────────────────────────── */

/* route: addr → 0/1/2  (base3 decision) */
static inline uint8_t rub_route3(uint64_t addr) {
    return (uint8_t)(((addr >> 1) ^ addr) % 3u);
}

/* parity: addr → 0/1  (base2 decision) */
static inline uint8_t rub_parity2(uint64_t addr) {
    return (uint8_t)(addr & 1u);
}

/* cell index: face + cell_in_face → bit offset in w[] */
static inline uint8_t rub_cell_offset(uint8_t face, uint8_t cell) {
    return (uint8_t)((face * RUB_CELLS_FACE + cell) * RUB_BITS_CELL);
}

/* read cell state (2 bits) */
static inline uint8_t rub_cell_get(const RubikState *rs,
                                    uint8_t face, uint8_t cell) {
    uint8_t  off  = rub_cell_offset(face, cell);
    uint8_t  word = off >> 6u;          /* which uint64              */
    uint8_t  bit  = off & 63u;          /* bit position in word      */
    return (uint8_t)((rs->w[word] >> bit) & 0x3u);
}

/* write cell state (2 bits) */
static inline void rub_cell_set(RubikState *rs,
                                 uint8_t face, uint8_t cell, uint8_t val) {
    uint8_t  off  = rub_cell_offset(face, cell);
    uint8_t  word = off >> 6u;
    uint8_t  bit  = off & 63u;
    rs->w[word]  &= ~((uint64_t)0x3u << bit);
    rs->w[word]  |=  ((uint64_t)(val & 0x3u) << bit);
}

/* ── rub_init: seed → initial state ─────────────────────────── */
/*
 * Each cell derives from seed via base2/3:
 *   cell_val = (derive(seed, face, cell) × 2) % 3 → {0,1,2} → 2bit
 */
static inline void rub_init(RubikState *rs, uint64_t seed) {
    rs->w[0]      = 0u;
    rs->w[1]      = 0u;
    rs->seed      = seed;
    rs->move_count = 0u;
    rs->axis      = 0u;
    rs->layer     = 0u;

    for (uint8_t f = 0; f < RUB_FACES; f++) {
        for (uint8_t c = 0; c < RUB_CELLS_FACE; c++) {
            uint64_t d   = derive_next_core(seed, f, c);
            uint8_t  val = (uint8_t)((d * 2u) % 3u);  /* base2×base3 */
            rub_cell_set(rs, f, c, val);
        }
    }
}

/* ── rub_move: apply one move (axis + layer, base3 driven) ──── */
/*
 * Move = rotate one layer of one axis
 * Rotation: cycle 4 face-strips (3 cells each)
 * Layer 0/1/2 maps to base3 routing decision
 *
 * Face cycle per axis (standard Rubik):
 *   X: F(0)→U(2)→B(5)→D(3) strips col[layer]
 *   Y: F(0)→R(1)→B(5)→L(4) strips row[layer]
 *   Z: U(2)→R(1)→D(3)→L(4) strips col[layer]
 */
static inline void rub_move(RubikState *rs, uint8_t axis, uint8_t layer) {
    if (axis >= RUB_AXES || layer >= RUB_LAYERS) return;

    /* face cycle tables — base3 indexed */
    static const uint8_t FACE_CYCLE[RUB_AXES][4] = {
        {0, 2, 5, 3},   /* X: F U B D */
        {0, 1, 5, 4},   /* Y: F R B L */
        {2, 1, 3, 4},   /* Z: U R D L */
    };

    /* strip cells per layer (col or row of 3) */
    /* base3: layer 0/1/2 → which strip */
    uint8_t strip[4][3];
    const uint8_t *fc = FACE_CYCLE[axis];

    /* read 4 face strips */
    for (uint8_t i = 0; i < 4u; i++)
        for (uint8_t j = 0; j < 3u; j++)
            strip[i][j] = rub_cell_get(rs, fc[i],
                          (axis == 1u) ? layer * 3u + j   /* row */
                                       : j * 3u + layer); /* col */

    /* rotate: i → i+1 (cycle 4) */
    uint8_t tmp[3];
    for (uint8_t j = 0; j < 3u; j++) tmp[j] = strip[3][j];
    for (uint8_t i = 3u; i > 0; i--)
        for (uint8_t j = 0; j < 3u; j++) strip[i][j] = strip[i-1][j];
    for (uint8_t j = 0; j < 3u; j++) strip[0][j] = tmp[j];

    /* write back */
    for (uint8_t i = 0; i < 4u; i++)
        for (uint8_t j = 0; j < 3u; j++)
            rub_cell_set(rs, fc[i],
                         (axis == 1u) ? layer * 3u + j
                                      : j * 3u + layer,
                         strip[i][j]);

    rs->axis       = axis;
    rs->layer      = layer;
    rs->move_count++;
}

/* ── rub_drive: addr → auto move (base2+base3 only) ─────────── */
/*
 * One addr → one deterministic move
 *   axis  = rub_parity2(addr >> 1) × ... mapped to 0..2
 *   layer = rub_route3(addr)        → 0..2
 * No lookup table — pure arithmetic
 */
static inline void rub_drive(RubikState *rs, uint64_t addr) {
    uint8_t axis  = (uint8_t)((addr % 3u));          /* base3 → axis  */
    uint8_t layer = (uint8_t)(rub_route3(addr >> 2)); /* base3 shifted */
    rub_move(rs, axis, layer);
}

/* ── rub_to_bundle: RubikState → 9-word bundle (ApexWire upstream) */
/*
 * Maps 54-cell state → 9 words for cpu_derive_bundle compatibility
 * w[0..1] = packed state (raw)
 * w[2]    = w[0] ^ w[1]
 * w[3]    = ~w[0]
 * w[4]    = ~w[1]
 * w[5]    = ~(w[0]^w[1])
 * w[6..8] = rotl(mix, 12/18/24)   ← sacred rotations
 */
static inline void rub_to_bundle(const RubikState *rs, uint64_t *b) {
    uint64_t mix = rs->w[0] ^ rs->w[1];
    b[0] = rs->w[0];
    b[1] = rs->w[1];
    b[2] = mix;
    b[3] = ~rs->w[0];
    b[4] = ~rs->w[1];
    b[5] = ~mix;
    b[6] = (mix << 12) | (mix >> (64u - 12u));  /* rotl 12         */
    b[7] = (mix << 18) | (mix >> (64u - 18u));  /* rotl 18 sacred  */
    b[8] = (mix << 24) | (mix >> (64u - 24u));  /* rotl 24 center  */
}

/* ── rub_checksum: fold all 54 cells → 32bit fingerprint ─────── */
static inline uint32_t rub_checksum(const RubikState *rs) {
    uint64_t fold = rs->w[0] ^ rs->w[1] ^ rs->seed;
    fold ^= (uint64_t)rs->move_count;
    return (uint32_t)(fold ^ (fold >> 32));
}

/* ── Compile-time checks ─────────────────────────────────────── */
typedef char _rub_cells_assert [(RUB_CELLS  == 54u) ? 1 : -1];
typedef char _rub_state_assert [(sizeof(RubikState) == 32u) ? 1 : -1];

#endif /* GEO_RUBIK_DRIVE_H */
