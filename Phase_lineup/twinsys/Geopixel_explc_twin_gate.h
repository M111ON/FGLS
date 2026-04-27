/*
 * lc_twin_gate.h — LetterCube Gate Layer for TPOGLS Twin Bridge
 * ═══════════════════════════════════════════════════════════════
 *
 * วางตรงกลางระหว่าง pipeline_wire_process → geo_fast_intersect
 * ใน twin_bridge_write hot path
 *
 * หน้าที่:
 *   รับ (addr, value, raw) จาก twin_bridge
 *   → encode เป็น LCHdr pair (A=addr-side, B=value-side)
 *   → ผ่าน lch_gate() → WARP / COLLISION / GROUND / FILTER_BLOCK
 *   → translate ผลกลับเป็น LCTwinDecision ที่ twin_bridge_write ใช้ตัดสิน
 *     ว่าจะ fast-path ไป geo_fast_intersect หรือ drop ลง dodeca ground
 *
 * Design rules (Frustum / LC DNA):
 *   - WARP      → ส่งตรง geo_fast_intersect (wireless link ข้าม geometry)
 *   - ROUTE     → ส่งปกติ geo_fast_intersect (normal path)
 *   - GROUND    → ข้าม geo_fast_intersect ทั้งหมด, dodeca_insert ทันที
 *                 (append-only ground lane, เหมือน LC ghost → residue zone)
 *   - COLLISION → เพิ่ม drift_acc (เป็น turbulence signal ใน diamond flow)
 *
 * ไม่ต้องการ malloc / heap / float
 * ═══════════════════════════════════════════════════════════════
 */

#ifndef LC_TWIN_GATE_H
#define LC_TWIN_GATE_H

#include <stdint.h>
#include <string.h>

/* LC headers — ใส่ path ตาม project layout */
#include "lc_hdr.h"
#include "lc_wire.h"

/* ══════════════════════════════════════════════════════
   GATE DECISION
   ══════════════════════════════════════════════════════ */
typedef enum {
    LC_TWIN_WARP      = 0,   /* fast path → geo_fast_intersect (bypass normal) */
    LC_TWIN_ROUTE     = 1,   /* normal    → geo_fast_intersect                 */
    LC_TWIN_GROUND    = 2,   /* skip geo  → dodeca_insert ทันที (ground lane)  */
    LC_TWIN_COLLISION = 3,   /* turbulence → drift++ + ส่งต่อ geo ปกติ         */
} LCTwinDecision;

/* ── ชื่อ decision สำหรับ debug ── */
static inline const char *lc_twin_decision_name(LCTwinDecision d) {
    switch (d) {
        case LC_TWIN_WARP:      return "WARP";
        case LC_TWIN_ROUTE:     return "ROUTE";
        case LC_TWIN_GROUND:    return "GROUND";
        case LC_TWIN_COLLISION: return "COLLISION";
        default:                return "?";
    }
}

/* ══════════════════════════════════════════════════════
   ENCODE: (addr, value, raw) → LCHdr pair
   ══════════════════════════════════════════════════════

   addr-side (node A):
     sign  = addr bit[63]
     mag   = popcount(addr & 0x7F)        ← 7-bit density, 0-63 → remap 1-127
     rgb   = addr bits [8:6][5:3][2:0]    ← low 9 bits → RGB3
     level = (raw >> 60) & 0x3            ← top 2 bits of raw → LC level
     angle = (raw >> 51) & 0x1FF          ← 9 bits → 0-511 angular
     face  = (addr ^ value) & 0x3         ← xor parity → face 0-3

   value-side (node B):
     complement of A: RGB = 7-rA,7-gA,7-bA, sign flipped
     — guarantees WARP if addr and value are "polar" to each other
     — guarantees COLLISION if same magnitude (symmetric write)
*/

static inline LCHdr lc_twin_encode_addr(uint64_t addr, uint64_t raw) {
    uint8_t  sign  = (uint8_t)((addr >> 63) & 1u);
    uint8_t  pop7  = (uint8_t)__builtin_popcountll(addr & 0x7Fu);
    uint8_t  mag   = (pop7 == 0) ? 1u : (uint8_t)(pop7 << 1);  /* 1-127, never ghost */
    if (mag > 127u) mag = 127u;
    uint8_t  r     = (uint8_t)((addr >> 6) & 0x7u);
    uint8_t  g     = (uint8_t)((addr >> 3) & 0x7u);
    uint8_t  b     = (uint8_t)( addr        & 0x7u);
    uint8_t  level = (uint8_t)((raw >> 62) & 0x3u);
    uint16_t angle = (uint16_t)((raw >> 51) & 0x1FFu);
    uint8_t  face  = 0u;   /* addr-side always face 0 */
    return lch_pack(sign, mag, r, g, b, level, angle, face);
}

static inline LCHdr lc_twin_encode_value(uint64_t value, uint64_t addr,
                                          uint64_t raw) {
    /*
     * Encode value ตรง ๆ จาก bits ของ value เอง (ไม่ใช่ complement สำเร็จรูป)
     * ปล่อยให้ lch_gate() judge ว่าเป็น WARP/COLLISION/GROUND เอง
     *
     * WARP เกิดเมื่อ: rgb_B = (7-r_A, 7-g_A, 7-b_A) AND sign_B != sign_A
     * → natural เมื่อ addr ^ value มี bit pattern ที่ polar กัน
     * COLLISION เกิดเมื่อ: rgb เหมือน + sign เหมือน (symmetric write)
     * GROUND เกิดเมื่อ: palette block หรือ mag=0 (ghost lane)
     */
    uint8_t  sign  = (uint8_t)((value >> 63) & 1u);
    uint8_t  pop7  = (uint8_t)__builtin_popcountll(value & 0x7Fu);
    uint8_t  mag   = (pop7 == 0) ? 1u : (uint8_t)(pop7 << 1);
    if (mag > 127u) mag = 127u;
    /* RGB: ใช้ bits [8:6][5:3][2:0] ของ value เหมือน hA */
    uint8_t  r     = (uint8_t)((value >> 6) & 0x7u);
    uint8_t  g     = (uint8_t)((value >> 3) & 0x7u);
    uint8_t  b     = (uint8_t)( value        & 0x7u);
    /* level/angle: มาจาก raw เหมือนกัน (same temporal context) */
    uint8_t  level = (uint8_t)((raw >> 62) & 0x3u);
    uint16_t angle = (uint16_t)((raw >> 51) & 0x1FFu);
    /* face: xor parity ของ addr^value → 0-3 */
    uint8_t  face  = (uint8_t)((addr ^ value) & 0x3u);
    return lch_pack(sign, mag, r, g, b, level, angle, face);
}

/* ══════════════════════════════════════════════════════
   GATE CONTEXT
   เก็บ LCPalette และ stats — ใส่ใน TwinBridge struct
   ══════════════════════════════════════════════════════ */
typedef struct {
    LCPalette  palette;       /* 4 face × 64-bit bitboard filter    */
    uint32_t   warp_count;    /* WARP decisions (wireless links)     */
    uint32_t   route_count;   /* ROUTE decisions (normal path)       */
    uint32_t   ground_count;  /* GROUND decisions (append-only drop) */
    uint32_t   collision_count; /* COLLISION (turbulence events)     */
} LCTwinGateCtx;

static inline void lc_twin_gate_init(LCTwinGateCtx *g) {
    memset(g, 0, sizeof(*g));
    /* default: all face bits open = ไม่ block อะไร */
    for (int i = 0; i < 4; i++)
        g->palette.mask[i] = ~0ull;
}

/* ── palette control ── */
static inline void lc_twin_gate_filter(LCTwinGateCtx *g,
                                        uint8_t face, uint16_t angle) {
    lch_palette_clear(&g->palette, face, angle);  /* block lane */
}

static inline void lc_twin_gate_open(LCTwinGateCtx *g,
                                      uint8_t face, uint16_t angle) {
    lch_palette_set(&g->palette, face, angle);    /* open lane */
}

/* ══════════════════════════════════════════════════════
   MAIN GATE CALL
   ใช้ใน twin_bridge_write ระหว่าง pipeline_wire_process
   และ geo_fast_intersect
   ══════════════════════════════════════════════════════ */
static inline LCTwinDecision lc_twin_gate(LCTwinGateCtx *g,
                                           uint64_t       addr,
                                           uint64_t       value,
                                           uint64_t       raw)
{
    LCHdr hA = lc_twin_encode_addr(addr, raw);
    LCHdr hB = lc_twin_encode_value(value, addr, raw);

    LCGate gate = lch_gate(hA, hB, &g->palette, &g->palette);

    LCTwinDecision d;
    switch (gate) {
        case LCG_WARP:
            d = LC_TWIN_WARP;
            g->warp_count++;
            break;
        case LCG_COLLISION:
            d = LC_TWIN_COLLISION;
            g->collision_count++;
            break;
        case LCG_GROUND_ABSORB:
            d = LC_TWIN_GROUND;
            g->ground_count++;
            break;
        case LCG_FILTER_BLOCK:
        default:
            d = LC_TWIN_ROUTE;
            g->route_count++;
            break;
    }
    return d;
}

/* ══════════════════════════════════════════════════════
   STATS PRINT
   ══════════════════════════════════════════════════════ */
static inline void lc_twin_gate_stats(const LCTwinGateCtx *g) {
    uint32_t total = g->warp_count + g->route_count
                   + g->ground_count + g->collision_count;
    if (total == 0) { printf("[LC_GATE] no ops\n"); return; }
    printf("[LC_GATE] total=%-6u  WARP=%-5u(%.1f%%)  ROUTE=%-5u(%.1f%%)"
           "  GROUND=%-5u(%.1f%%)  COLLISION=%-5u(%.1f%%)\n",
           total,
           g->warp_count,      100.f * g->warp_count      / total,
           g->route_count,     100.f * g->route_count     / total,
           g->ground_count,    100.f * g->ground_count    / total,
           g->collision_count, 100.f * g->collision_count / total);
}

#endif /* LC_TWIN_GATE_H */

/*
 * ══════════════════════════════════════════════════════
 * HOW TO WIRE INTO twin_bridge_write:
 *
 *  1. เพิ่มใน TwinBridge struct:
 *       LCTwinGateCtx  lc_gate;
 *
 *  2. เพิ่มใน twin_bridge_init:
 *       lc_twin_gate_init(&b->lc_gate);
 *
 *  3. ใน twin_bridge_write หลัง pipeline_wire_process:
 *
 *       LCTwinDecision lc_dec =
 *           lc_twin_gate(&b->lc_gate, addr, value, raw);
 *
 *       if (lc_dec == LC_TWIN_GROUND) {
 *           // ข้าม geo_fast_intersect ทั้งหมด
 *           uint8_t offset = (uint8_t)(raw & 0xFFu);
 *           dodeca_insert(&b->dodeca, raw & UINT64_C(0x00FFFFFFFFFFFFFF),
 *                         0, 0, offset, 0, 0);
 *           b->twin_writes++;
 *           diamond_flow_init(&b->flow);
 *           b->total_ops++;
 *           return ev;
 *       }
 *
 *       if (lc_dec == LC_TWIN_COLLISION)
 *           b->flow.drift_acc += 8u;   // turbulence bump
 *
 *       // ส่งต่อ geo_fast_intersect ปกติ (WARP และ ROUTE)
 *       uint64_t isect = geo_fast_intersect(core_raw);
 *       ...
 *
 * ══════════════════════════════════════════════════════
 */
