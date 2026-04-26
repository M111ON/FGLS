/*
 * geo_apex_wire.h — FGLS Apex 4-Wire + LetterCube Coset Routing
 * ══════════════════════════════════════════════════════════════
 *
 * Apex = 4 logical wires (symmetry preserved — no fan-out at source)
 *   wire[0] hilbert_a    → World A address (PHI_UP)
 *   wire[1] hilbert_b    → World B address (PHI_DOWN)
 *   wire[2] hilbert_quad → 4-path quad mirror packed (8bit × 4)
 *   wire[3] route/mask   → base3 route + 3-bit invert_mask
 *
 * Hilbert quad mirror rule:
 *   4 explicit paths cover space
 *   1 side always skipped per set → invert → 3 implicit paths
 *   4 explicit + 3 inverted + hilbert_a + hilbert_b = 9 paths ✅
 *
 * Address architecture:
 *   hilbert_quad  = GPS (routing only, 8bit precision ok)
 *   LetterCube    = real address (6! = 720 unique permutations)
 *   720 × 6 faces × 4 levels = 17,280 unique addresses per GiantCube
 *
 * Speed: derive_next_core = 2-cycle (mix→rotl→mix)
 *   n is self-contained — derived from seed + dispatch_id
 *   no external state, no pointer, no malloc
 *
 * Dispatch math (frozen):
 *   12 coset × 6 frustum × 64B = 4,608B per apex dispatch
 *   4,608 ÷ 32 threads          = 144 = F(12) = 1 fibo clock ✅
 *
 * No malloc, no float, no heap.
 * ══════════════════════════════════════════════════════════════
 */

#ifndef GEO_APEX_WIRE_H
#define GEO_APEX_WIRE_H

#include <stdint.h>
#include "pogls_fibo_addr.h"
#include "geo_primitives.h"
#include "geo_letter_cube.h"

/* ── Constants ───────────────────────────────────────────────── */
#define APEX_COSET_COUNT      12u
#define APEX_FRUSTUM_MOUNT     6u
#define APEX_SLOT_BYTES       64u
#define APEX_DISPATCH_BYTES   (APEX_COSET_COUNT * APEX_FRUSTUM_MOUNT * APEX_SLOT_BYTES)
#define APEX_THREAD_COUNT     32u
#define APEX_FIBO_CLOCK       (APEX_DISPATCH_BYTES / APEX_THREAD_COUNT)
                                     /* = 144 = F(12) ✅                */
#define APEX_QUAD_PATHS        4u
#define APEX_INVERT_PATHS      3u
#define APEX_TOTAL_PATHS       9u    /* 4 explicit + 3 inverted + a + b */
#define APEX_PERM_SPACE      720u    /* 6! = LetterCube address space   */
#define APEX_ADDR_TOTAL    (APEX_PERM_SPACE * APEX_FRUSTUM_MOUNT * 4u)
                                     /* = 17,280 unique addresses ✅    */

/* ── ApexWire: 16B ───────────────────────────────────────────── */
typedef struct {
    uint32_t hilbert_a;      /* World A fibo address (PHI_UP)        */
    uint32_t hilbert_b;      /* World B fibo address (PHI_DOWN)      */
    uint32_t hilbert_quad;   /* 4 quad paths packed (8bit × 4)       */
    uint8_t  route;          /* base3: 0=pair 1=branch 2=skip        */
    uint8_t  invert_mask;    /* 3-bit: hilbert skip-side map         */
    uint8_t  letter_upper;   /* LetterPair upper 0..25               */
    uint8_t  letter_lower;   /* LetterPair lower 0..25               */
} ApexWire;                  /* 16B = cache-line ÷ 4 ✅              */

typedef char _apex_wire_size_assert[(sizeof(ApexWire) == 16u) ? 1 : -1];

/* ── apex_wire_build ─────────────────────────────────────────── */
/*
 * n = self-contained, derived from seed + dispatch_id
 * quad: derive_next_core (2-cycle) >> 3 → strip low entropy → % PHI_SCALE
 * invert_mask: XOR fold of quad → 3-bit skip indicator
 */
static inline ApexWire apex_wire_build(uint64_t seed, uint32_t dispatch_id)
{
    ApexWire w;

    uint32_t n    = (uint32_t)(
        derive_next_core(seed, 0u, dispatch_id) % PHI_SCALE
    );

    w.hilbert_a   = fibo_addr(n, 0u, 0u);
    w.hilbert_b   = fibo_addr(n, 0u, 1u);

    uint32_t q[APEX_QUAD_PATHS];
    for (uint8_t i = 0; i < APEX_QUAD_PATHS; i++)
        q[i] = (uint32_t)((derive_next_core(seed, i, n) >> 3) % PHI_SCALE) & 0xFFu;

    w.hilbert_quad = q[0] | (q[1] << 8) | (q[2] << 16) | (q[3] << 24);
    w.invert_mask  = (uint8_t)((q[0] ^ q[1] ^ q[2] ^ q[3]) & 0x7u);

    uint32_t fold  = (uint32_t)(seed ^ (seed >> 32)) ^ dispatch_id;
    w.route        = (uint8_t)(fold % 3u);

    uint64_t lseed = derive_next_core(seed, 4u, n);
    w.letter_upper = (uint8_t)((lseed >> 0) % LC_PAIRS);
    w.letter_lower = (uint8_t)((lseed >> 8) % LC_PAIRS);
    return w;
}

/* ── apex_quad_path: unpack explicit quad path 0..3 ─────────── */
static inline uint8_t apex_quad_path(const ApexWire *w, uint8_t idx)
{
    if (idx >= APEX_QUAD_PATHS) return 0u;
    return (uint8_t)((w->hilbert_quad >> (idx * 8u)) & 0xFFu);
}

/* ── apex_invert_path: derive inverted (skip-side) path 0..2 ── */
/*
 * Hilbert rule: 1 side skipped per set
 * invert_mask bit[i]=1 → that quad is the skip → complement it
 */
static inline uint8_t apex_invert_path(const ApexWire *w, uint8_t idx)
{
    if (idx >= APEX_INVERT_PATHS) return 0u;
    uint8_t q       = apex_quad_path(w, idx);
    uint8_t is_skip = (w->invert_mask >> idx) & 1u;
    return is_skip ? (~q & 0xFFu) : q;
}

/* ── lc_perm_address: full LetterCube address 0..719 ────────── */
static inline uint32_t lc_perm_address(const ApexWire *w)
{
    uint32_t fold = (w->hilbert_a   ^ w->hilbert_b)
                  ^  w->hilbert_quad
                  ^ ((uint32_t)w->route        << 20)
                  ^ ((uint32_t)w->letter_upper <<  3);
    return fold % APEX_PERM_SPACE;
}

/* ── lc_route_coset: perm → coset 0..11 ─────────────────────── */
static inline uint8_t lc_route_coset(const ApexWire *w)
{
    return (uint8_t)(lc_perm_address(w) / 60u);
}

/* ── apex_dispatch_offset: byte offset into 4608B payload ────── */
static inline uint32_t apex_dispatch_offset(uint8_t coset, uint8_t frustum)
{
    if (coset >= APEX_COSET_COUNT || frustum >= APEX_FRUSTUM_MOUNT) return 0u;
    return (uint32_t)coset   * (APEX_FRUSTUM_MOUNT * APEX_SLOT_BYTES)
         + (uint32_t)frustum *  APEX_SLOT_BYTES;
}

/* ── apex_letter_valid ───────────────────────────────────────── */
static inline int apex_letter_valid(const ApexWire *w)
{
    return (w->letter_upper < LC_PAIRS)
        && (w->letter_lower < LC_PAIRS)
        && (w->letter_upper == w->letter_lower);
}

/* ── Compile-time guards ─────────────────────────────────────── */
typedef char _apex_fibo_assert  [(APEX_FIBO_CLOCK  == 144u)   ? 1 : -1];
typedef char _apex_paths_assert [(APEX_TOTAL_PATHS ==   9u)   ? 1 : -1];
typedef char _apex_addr_assert  [(APEX_ADDR_TOTAL  == 17280u) ? 1 : -1];

#endif /* GEO_APEX_WIRE_H */
