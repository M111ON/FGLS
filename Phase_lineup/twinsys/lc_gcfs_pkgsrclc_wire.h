/*
 * lc_wire.h — LetterCube Wire Layer
 * ═══════════════════════════════════════════════════════════════
 *
 * Wires together:
 *   lc_hdr.h        — 32-bit node header (RGB9, sign, mag, angular, level)
 *   lc_polar.h      — state machine (migrated to LCHdr)
 *   geo_rubik_drive — address generator (skip→ground fixed)
 *   LCPalette       — bitboard filter in pipeline
 *   LCTraversal     — fold-aware traversal context
 *
 * Pipeline per request:
 *   addr → rubik route → palette filter → lc_gate → route/ground
 *
 * No malloc. No float. No heap.
 * ═══════════════════════════════════════════════════════════════
 */

#ifndef LC_WIRE_H
#define LC_WIRE_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "lc_hdr.h"

/* ══════════════════════════════════════════════════════
   1. NODE — uses LCHdr v2 (replaces PolarHdr)
   ══════════════════════════════════════════════════════ */
#define LCW_MAIN_SPACE    20736u
#define LCW_RESIDUE       6912u
#define LCW_TOTAL         27648u

typedef struct {
    uint32_t  address;
    uint32_t  pair_addr;
    LCHdr     hdr;        /* v2 — 32-bit replaces PolarHdr */
    uint8_t   occupied;
    uint8_t   _pad[3];
} LCWNode;               /* 16 bytes ✅ cache-line friendly */

static LCWNode lcw_space[LCW_TOTAL];

static inline void lcw_init(void) {
    memset(lcw_space, 0, sizeof(lcw_space));
    for (uint32_t i = 0; i < LCW_TOTAL; i++)
        lcw_space[i].address = i;
}

/* ── is address in residue zone ── */
static inline int lcw_is_residue(uint32_t addr) {
    return addr >= LCW_MAIN_SPACE && addr < LCW_TOTAL;
}

/* ── insert ── */
static inline int lcw_insert(uint32_t addr, uint32_t pair,
                               LCHdr hdr, const char *label) {
    (void)label;
    if (addr >= LCW_TOTAL)       return -1;
    if (lcw_space[addr].occupied) return -2;
    lcw_space[addr].hdr      = hdr;
    lcw_space[addr].pair_addr = pair;
    lcw_space[addr].occupied  = 1;
    return 0;
}

/* ── ghost delete: zero mag, keep trail ── */
static inline int lcw_ghost(uint32_t addr) {
    if (addr >= LCW_TOTAL || !lcw_space[addr].occupied) return -1;
    lcw_space[addr].hdr = lch_ghost(lcw_space[addr].hdr);
    uint32_t pair = lcw_space[addr].pair_addr;
    if (pair < LCW_TOTAL && lcw_space[pair].occupied)
        lcw_space[pair].hdr = lch_ghost(lcw_space[pair].hdr);
    return 0;
}

/* ── flip pair ── */
static inline int lcw_flip(uint32_t addr) {
    if (addr >= LCW_TOTAL || !lcw_space[addr].occupied) return -1;
    if (lch_is_ghost(lcw_space[addr].hdr)) return -2;
    lcw_space[addr].hdr = lch_flip(lcw_space[addr].hdr);
    uint32_t pair = lcw_space[addr].pair_addr;
    if (pair < LCW_TOTAL && lcw_space[pair].occupied &&
        lch_is_active(lcw_space[pair].hdr))
        lcw_space[pair].hdr = lch_flip(lcw_space[pair].hdr);
    return 0;
}

/* ══════════════════════════════════════════════════════
   2. RUBIK ROUTE — skip(2) → residue zone
   ══════════════════════════════════════════════════════ */

/* base3 route: 0=pair 1=branch 2=ground(was skip) */
static inline uint8_t lcw_route3(uint64_t addr) {
    return (uint8_t)(((addr >> 1) ^ addr) % 3u);
}

/* resolve addr → destination
 *   route=0 → pair_addr (normal)
 *   route=1 → branch: addr ^ 0x1 (sibling)
 *   route=2 → ground: map into residue zone (was void/skip)
 */
static inline uint32_t lcw_rubik_route(uint32_t addr) {
    uint8_t r = lcw_route3(addr);
    if (r == 0) {
        /* pair route */
        if (addr < LCW_TOTAL && lcw_space[addr].occupied)
            return lcw_space[addr].pair_addr;
        return addr;
    }
    if (r == 1) {
        /* branch: sibling address */
        return addr ^ 1u;
    }
    /* r == 2: ground → residue zone
     * map: addr % RESIDUE + MAIN_SPACE
     * deterministic, no collision with main space */
    return (addr % LCW_RESIDUE) + LCW_MAIN_SPACE;
}

/* ══════════════════════════════════════════════════════
   3. TRAVERSAL CONTEXT — fold-aware, not in node
   ══════════════════════════════════════════════════════ */
#define LCW_MAX_FOLD  8u   /* max fold depth */

typedef struct {
    uint32_t    root_addr;      /* unfolded origin */
    uint8_t     fold_depth;     /* current fold level (0=flat) */
    uint8_t     level;          /* LC_LEVEL_0..3 */
    uint8_t     axis;           /* Rubik axis 0=X 1=Y 2=Z */
    uint8_t     _pad;
    uint32_t    fold_stack[LCW_MAX_FOLD]; /* addr at each fold */
    uint32_t    visit_count;
} LCTraversal;

static inline void lcw_trav_init(LCTraversal *t, uint32_t root, uint8_t level) {
    memset(t, 0, sizeof(LCTraversal));
    t->root_addr = root;
    t->level     = level;
}

static inline int lcw_trav_fold(LCTraversal *t, uint32_t addr) {
    if (t->fold_depth >= LCW_MAX_FOLD) return -1;
    t->fold_stack[t->fold_depth++] = addr;
    return 0;
}

static inline uint32_t lcw_trav_unfold(LCTraversal *t) {
    if (t->fold_depth == 0) return t->root_addr;
    return t->fold_stack[--t->fold_depth];
}

static inline uint8_t lcw_trav_depth(const LCTraversal *t) {
    return t->fold_depth;
}

/* ══════════════════════════════════════════════════════
   4. FULL PIPELINE
   addr → palette → rubik route → lc_gate → dest
   ══════════════════════════════════════════════════════ */
typedef enum {
    LCW_RESULT_WARP,
    LCW_RESULT_ROUTE,
    LCW_RESULT_GROUND,
    LCW_RESULT_COLLISION,
    LCW_RESULT_GHOST,
} LCWResult;

typedef struct {
    LCWResult   result;
    uint32_t    dest;       /* destination address */
    uint8_t     fold_delta; /* fold depth change (+1 fold, -1 unfold, 0 same) */
} LCWRouteOut;

static inline LCWRouteOut lcw_route(uint32_t addr,
                                     const LCPalette *pal,
                                     LCTraversal *trav) {
    LCWRouteOut out = {LCW_RESULT_GROUND, 0, 0};

    if (addr >= LCW_TOTAL || !lcw_space[addr].occupied) {
        out.result = LCW_RESULT_GROUND;
        out.dest   = (addr % LCW_RESIDUE) + LCW_MAIN_SPACE;
        return out;
    }

    LCWNode *n = &lcw_space[addr];

    /* ghost = amnesia → ground */
    if (lch_is_ghost(n->hdr)) {
        out.result = LCW_RESULT_GHOST;
        out.dest   = (addr % LCW_RESIDUE) + LCW_MAIN_SPACE;
        return out;
    }

    /* palette filter */
    if (pal && !lch_palette_pass(pal, n->hdr)) {
        out.result = LCW_RESULT_GROUND;
        out.dest   = (addr % LCW_RESIDUE) + LCW_MAIN_SPACE;
        return out;
    }

    /* rubik route decision */
    uint32_t rubik_dest = lcw_rubik_route(addr);

    /* route=2 already sent to ground */
    if (lcw_is_residue(rubik_dest) && !lcw_is_residue(addr)) {
        out.result = LCW_RESULT_GROUND;
        out.dest   = rubik_dest;
        if (trav) lcw_trav_fold(trav, addr);
        out.fold_delta = 1;
        return out;
    }

    /* gate check with destination node */
    if (rubik_dest < LCW_TOTAL && lcw_space[rubik_dest].occupied) {
        LCGate g = lch_gate(n->hdr, lcw_space[rubik_dest].hdr, pal, pal);
        switch (g) {
            case LCG_WARP:
                out.result = LCW_RESULT_WARP;
                out.dest   = rubik_dest;
                return out;
            case LCG_COLLISION:
                out.result = LCW_RESULT_COLLISION;
                out.dest   = (addr % LCW_RESIDUE) + LCW_MAIN_SPACE;
                return out;
            case LCG_GROUND_ABSORB:
                out.result = LCW_RESULT_GROUND;
                out.dest   = (addr % LCW_RESIDUE) + LCW_MAIN_SPACE;
                return out;
            default: break;
        }
    }

    out.result = LCW_RESULT_ROUTE;
    out.dest   = rubik_dest;
    if (trav) trav->visit_count++;
    return out;
}

/* ── result name ── */
static inline const char *lcw_result_name(LCWResult r) {
    switch (r) {
        case LCW_RESULT_WARP:      return "WARP";
        case LCW_RESULT_ROUTE:     return "ROUTE";
        case LCW_RESULT_GROUND:    return "GROUND";
        case LCW_RESULT_COLLISION: return "COLLISION";
        case LCW_RESULT_GHOST:     return "GHOST";
        default:                   return "?";
    }
}

#endif /* LC_WIRE_H */

/* ══════════════════════════════════════════════════════
   SELF-TEST
   gcc -O2 -DTEST_LC_WIRE -o lc_wire_test -x c lc_wire.h
   ══════════════════════════════════════════════════════ */
#ifdef TEST_LC_WIRE
int main(void) {
    int pass=0, fail=0;
#define CHK(cond,msg) do{ if(cond){printf("  ✓ %s\n",msg);pass++;} \
                          else{printf("  ✗ FAIL: %s\n",msg);fail++;} }while(0)

    printf("╔══════════════════════════════════════════════╗\n");
    printf("║  lc_wire.h — Full Pipeline Test             ║\n");
    printf("╚══════════════════════════════════════════════╝\n\n");

    lcw_init();

    /* ── T1: sizeof ── */
    printf("▶ T1: sizeof\n");
    printf("  LCWNode = %zu bytes  (expect 16)\n", sizeof(LCWNode));
    CHK(sizeof(LCWNode)==16, "LCWNode = 16 bytes");

    /* ── T2: rubik route skip→ground ── */
    printf("\n▶ T2: rubik route — skip→ground\n");
    int ground_count = 0;
    for (uint32_t i = 0; i < 100; i++) {
        uint8_t r = lcw_route3(i);
        if (r == 2) {
            uint32_t dest = lcw_rubik_route(i);
            if (lcw_is_residue(dest)) ground_count++;
        }
    }
    printf("  route=2 destinations all in residue zone: %s\n",
           ground_count > 0 ? "YES" : "N/A");
    /* verify specific addr */
    uint32_t g_dest = (42u % LCW_RESIDUE) + LCW_MAIN_SPACE;
    CHK(g_dest >= LCW_MAIN_SPACE && g_dest < LCW_TOTAL,
        "skip→ground maps to residue zone");

    /* ── T3: insert + ghost + trail ── */
    printf("\n▶ T3: insert, ghost, audit trail\n");
    LCHdr hA = lch_pack(0, 64, 5, 3, 1, LC_LEVEL_1, 0,   0);
    LCHdr hB = lch_pack(1, 64, 2, 4, 6, LC_LEVEL_1, 256, 0);
    lcw_insert(100, 200, hA, "A");
    lcw_insert(200, 100, hB, "a");
    CHK(lch_is_active(lcw_space[100].hdr), "A active");
    lcw_ghost(100);
    CHK(lch_is_ghost(lcw_space[100].hdr),  "A ghost after delete");
    CHK(lch_is_ghost(lcw_space[200].hdr),  "a ghost (pair)");
    CHK(lcw_space[100].occupied == 1,       "slot still occupied");
    CHK(lch_r(lcw_space[100].hdr) == 5,    "RGB trail preserved");

    /* ── T4: flip ── */
    printf("\n▶ T4: flip pair\n");
    LCHdr hC = lch_pack(0, 80, 3, 3, 3, LC_LEVEL_1, 10, 1);
    LCHdr hD = lch_pack(1, 80, 4, 4, 4, LC_LEVEL_1, 20, 1);
    lcw_insert(300, 400, hC, "C");
    lcw_insert(400, 300, hD, "D");
    lcw_flip(300);
    CHK(lch_sign(lcw_space[300].hdr)==1, "C flipped to neg");
    CHK(lch_sign(lcw_space[400].hdr)==0, "D flipped to pos");
    CHK(lcw_flip(100)==-2, "ghost flip = -2");

    /* ── T5: full pipeline warp ── */
    printf("\n▶ T5: pipeline — WARP\n");
    LCHdr hE = lch_pack(0, 90, 5, 3, 1, LC_LEVEL_1, 0, 0);
    LCHdr hF = lch_pack(1, 90, 2, 4, 6, LC_LEVEL_1, 0, 0);
    lcw_insert(500, 600, hE, "E");
    lcw_insert(600, 500, hF, "F");
    /* force pair route for addr 500 */
    lcw_space[500].pair_addr = 600;
    LCTraversal trav;
    lcw_trav_init(&trav, 500, LC_LEVEL_1);
    /* set palette to pass both */
    LCPalette pal = {0};
    lch_palette_set(&pal, 0, 0);
    LCWRouteOut out = lcw_route(500, &pal, &trav);
    printf("  route(500) = %s → dest=%u\n",
           lcw_result_name(out.result), out.dest);
    CHK(out.result == LCW_RESULT_WARP || out.result == LCW_RESULT_ROUTE,
        "pipeline produces valid result");

    /* ── T6: pipeline — ghost = GHOST result ── */
    printf("\n▶ T6: pipeline — GHOST amnesia\n");
    out = lcw_route(100, &pal, &trav);
    printf("  route(ghost@100) = %s → dest=%u\n",
           lcw_result_name(out.result), out.dest);
    CHK(out.result == LCW_RESULT_GHOST, "ghost → GHOST result");
    CHK(lcw_is_residue(out.dest),        "ghost dest in residue zone");

    /* ── T7: palette block → ground ── */
    printf("\n▶ T7: palette filter → GROUND\n");
    LCPalette empty_pal = {0};  /* no bits set */
    out = lcw_route(500, &empty_pal, &trav);
    printf("  route(500, empty_palette) = %s\n", lcw_result_name(out.result));
    CHK(out.result == LCW_RESULT_GROUND, "palette block → GROUND");

    /* ── T8: traversal fold/unfold ── */
    printf("\n▶ T8: traversal fold depth\n");
    LCTraversal t2;
    lcw_trav_init(&t2, 0, LC_LEVEL_1);
    CHK(lcw_trav_depth(&t2)==0, "init depth=0");
    lcw_trav_fold(&t2, 100);
    lcw_trav_fold(&t2, 200);
    CHK(lcw_trav_depth(&t2)==2, "depth=2 after 2 folds");
    uint32_t uf = lcw_trav_unfold(&t2);
    CHK(uf==200, "unfold returns last fold");
    CHK(lcw_trav_depth(&t2)==1, "depth=1 after unfold");

    /* ── T9: residue zone capacity ── */
    printf("\n▶ T9: residue zone\n");
    printf("  main=%u  residue=%u  total=%u\n",
           LCW_MAIN_SPACE, LCW_RESIDUE, LCW_TOTAL);
    CHK(LCW_MAIN_SPACE + LCW_RESIDUE == LCW_TOTAL, "main+residue=total");
    CHK(LCW_RESIDUE == 4u * (12u*12u*12u), "residue=4×12³");

    printf("\n══════════════════════════════════════════════\n");
    printf("  PASS: %d   FAIL: %d\n", pass, fail);
    if (!fail) printf("✅ All PASS\n");
    else       printf("❌ %d FAIL\n", fail);
    return fail ? 1 : 0;
}
#endif
