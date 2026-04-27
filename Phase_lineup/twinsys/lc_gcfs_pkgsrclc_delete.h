/*
 * lc_delete.h — Atomic Triple-Cut Delete
 * ═══════════════════════════════════════════════════════════════
 *
 * delete = three simultaneous cuts:
 *   1. node.state  → 0      (cut current)
 *   2. rewind.state → GHOST (cut temporal reconstruct)
 *   3. rewind.ref   → 0     (cut index brute-force)
 *
 * All three happen in lcfs_delete_atomic() — no gap between cuts.
 * Attacker must break all three independently; no partial recovery.
 *
 *   "เห็นว่าเคยมี แต่ reconstruct ไม่ได้ 100%"
 *
 * Depends on: lc_fs.h, geo_rewind.h (RewindBuffer)
 * No malloc. No float. No heap.
 * ═══════════════════════════════════════════════════════════════
 */

#ifndef LC_DELETE_H
#define LC_DELETE_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "lc_fs.h"

/* ── minimal RewindSlot stub (full version from geo_rewind.h) ──
 * If geo_rewind.h is included before this file, the real struct
 * is used. Otherwise this stub provides the ghost interface.     */
#ifndef GEO_REWIND_H
#define REWIND_SLOTS 972u

typedef struct {
    uint32_t  enc;      /* GEO_WALK enc — 0 = empty           */
    uint8_t   ghosted;  /* LC ghost flag — added by lc_delete  */
    uint8_t   valid;    /* rewind valid flag                   */
    uint8_t   _pad[2];
    /* TStreamChunk chunk omitted in stub — real version has it */
} RewindSlot;

typedef struct {
    RewindSlot slots[REWIND_SLOTS];
    uint16_t   head;
    uint32_t   stored;
} RewindBuffer;
#endif /* GEO_REWIND_H */

/* ══════════════════════════════════════════════════════
   REWIND GHOST — cut both state and ref
   ══════════════════════════════════════════════════════ */

/* mark all rewind slots that reference addr_a or addr_b as ghost */
static inline uint32_t lc_rewind_ghost(RewindBuffer *rb,
                                        uint32_t addr_a,
                                        uint32_t addr_b) {
    if (!rb) return 0;
    uint32_t cut_count = 0;

    for (uint32_t i = 0; i < REWIND_SLOTS; i++) {
        RewindSlot *s = &rb->slots[i];
        if (!s->valid) continue;

        /* enc encodes address — match either node of the pair */
        uint32_t enc_a = addr_a ^ 0xA5A5u;  /* same XOR used at insert time */
        uint32_t enc_b = addr_b ^ 0xA5A5u;

        if (s->enc == enc_a || s->enc == enc_b) {
            s->ghosted = 1;   /* cut 2: state → ghost */
            s->enc     = 0;   /* cut 3: ref  → null   */
            s->valid   = 0;   /* entry no longer reconstructable */
            cut_count++;
        }
    }
    return cut_count;
}

/* ══════════════════════════════════════════════════════
   ATOMIC TRIPLE-CUT DELETE
   All three cuts happen before function returns
   ══════════════════════════════════════════════════════ */
typedef struct {
    int      status;        /* 0=ok, negative=error */
    uint32_t chunks_ghosted;
    uint32_t rewind_cut;
    uint8_t  append_only_verified; /* slots still occupied */
} LCDeleteResult;

static inline LCDeleteResult lcfs_delete_atomic(int fslot,
                                                  RewindBuffer *rb) {
    LCDeleteResult res = {0, 0, 0, 0};

    if (fslot < 0 || fslot >= (int)LCFS_MAX_FILES) {
        res.status = -1;
        return res;
    }
    LCFSFile *f = &lcfs_files[fslot];
    if (!f->open) {
        res.status = -2;
        return res;
    }

    for (uint32_t i = 0; i < f->chunk_count; i++) {
        LCFSChunk *c = &f->chunks[i];
        if (c->ghosted) continue;

        /* ── CUT 1: node state → 0 ── */
        lcw_ghost(c->addr_a);   /* ghosts both addr_a and addr_b (pair) */
        c->ghosted = 1;
        res.chunks_ghosted++;

        /* ── CUT 2+3: rewind ghost + ref null ── */
        res.rewind_cut += lc_rewind_ghost(rb, c->addr_a, c->addr_b);
    }

    /* verify append-only: all addr_a slots still occupied */
    res.append_only_verified = 1;
    for (uint32_t i = 0; i < f->chunk_count; i++) {
        uint32_t a = lcfs_files[fslot].chunks[i].addr_a;
        if (a < LCW_TOTAL && !lcw_space[a].occupied) {
            res.append_only_verified = 0;
            break;
        }
    }

    res.status = 0;
    return res;
}

/* ══════════════════════════════════════════════════════
   AUDIT: what's left after delete
   ══════════════════════════════════════════════════════ */
typedef struct {
    uint32_t ghost_nodes;       /* LC nodes with state=0 */
    uint32_t occupied_slots;    /* address slots still taken */
    uint32_t rewind_ghosts;     /* rewind entries ghosted */
    uint32_t rewind_live;       /* rewind entries still live */
    uint8_t  rgb_trail_intact;  /* RGB preserved in ghost nodes */
} LCDeleteAudit;

static inline LCDeleteAudit lcfs_audit(int fslot, const RewindBuffer *rb) {
    LCDeleteAudit a = {0};
    if (fslot < 0 || fslot >= (int)LCFS_MAX_FILES) return a;
    LCFSFile *f = &lcfs_files[fslot];
    if (!f->open) return a;

    a.rgb_trail_intact = 1;
    for (uint32_t i = 0; i < f->chunk_count; i++) {
        uint32_t addr = f->chunks[i].addr_a;
        if (addr >= LCW_TOTAL) continue;

        if (lcw_space[addr].occupied) a.occupied_slots++;
        if (lch_is_ghost(lcw_space[addr].hdr)) {
            a.ghost_nodes++;
            /* check RGB trail — at least one channel should be non-zero */
            if (lch_r(lcw_space[addr].hdr) == 0 &&
                lch_g(lcw_space[addr].hdr) == 0 &&
                lch_b(lcw_space[addr].hdr) == 0)
                a.rgb_trail_intact = 0;
        }
    }

    if (rb) {
        for (uint32_t i = 0; i < REWIND_SLOTS; i++) {
            if (rb->slots[i].ghosted) a.rewind_ghosts++;
            else if (rb->slots[i].valid) a.rewind_live++;
        }
    }

    return a;
}

static inline void lcfs_audit_print(const LCDeleteAudit *a) {
    printf("  ghost_nodes=%-3u  occupied=%-3u  "
           "rw_ghost=%-3u  rw_live=%-3u  rgb_trail=%s\n",
           a->ghost_nodes, a->occupied_slots,
           a->rewind_ghosts, a->rewind_live,
           a->rgb_trail_intact ? "OK" : "WARN");
}

#endif /* LC_DELETE_H */

/* ══════════════════════════════════════════════════════
   SELF-TEST
   gcc -O2 -DTEST_LC_DELETE -o lc_delete_test -x c lc_delete.h
   ══════════════════════════════════════════════════════ */
#ifdef TEST_LC_DELETE
int main(void) {
    int pass=0, fail=0;
#define CHK(cond,msg) do{ if(cond){printf("  ✓ %s\n",msg);pass++;} \
                          else{printf("  ✗ FAIL: %s\n",msg);fail++;} }while(0)

    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║  lc_delete.h — Triple-Cut Delete Test           ║\n");
    printf("╚══════════════════════════════════════════════════╝\n\n");

    lcfs_init();

    /* setup rewind buffer */
    static RewindBuffer rb;
    memset(&rb, 0, sizeof(rb));

    /* open file */
    uint32_t sizes[4] = {4896, 4896, 4896, 2048};
    uint64_t seeds[4] = {0x1111, 0x2222, 0x3333, 0x4444};
    int fd = lcfs_open("secret.bin", 4, sizes, seeds, LC_LEVEL_1);
    CHK(fd >= 0, "file opened");

    /* simulate rewind entries for chunks 0 and 1 */
    uint32_t a0 = lcfs_files[fd].chunks[0].addr_a;
    uint32_t b0 = lcfs_files[fd].chunks[0].addr_b;
    uint32_t a1 = lcfs_files[fd].chunks[1].addr_a;
    rb.slots[0].enc     = a0 ^ 0xA5A5u;
    rb.slots[0].valid   = 1;
    rb.slots[0].ghosted = 0;
    rb.slots[1].enc     = b0 ^ 0xA5A5u;
    rb.slots[1].valid   = 1;
    rb.slots[1].ghosted = 0;
    rb.slots[2].enc     = a1 ^ 0xA5A5u;
    rb.slots[2].valid   = 1;
    rb.slots[2].ghosted = 0;
    rb.stored = 3;

    /* verify readable before delete */
    LCPalette pal = {0};
    for (int f=0; f<4; f++) pal.mask[f] = ~0ull;
    CHK(lcfs_read(fd, 0, &pal) != NULL, "chunk 0 readable before delete");
    CHK(lcfs_read(fd, 1, &pal) != NULL, "chunk 1 readable before delete");

    printf("\n▶ Atomic triple-cut delete\n");
    LCDeleteResult res = lcfs_delete_atomic(fd, &rb);
    printf("  status=%d  chunks_ghosted=%u  rewind_cut=%u  append_only=%u\n",
           res.status, res.chunks_ghosted, res.rewind_cut,
           res.append_only_verified);

    CHK(res.status == 0,              "delete status OK");
    CHK(res.chunks_ghosted == 4,      "all 4 chunks ghosted");
    CHK(res.rewind_cut >= 2,          "rewind entries cut");
    CHK(res.append_only_verified == 1,"append-only preserved");

    /* ── verify cut 1: node state = 0 ── */
    printf("\n▶ Cut 1: node state\n");
    CHK(lcfs_read(fd, 0, &pal) == NULL, "chunk 0 unreadable (cut 1)");
    CHK(lcfs_read(fd, 1, &pal) == NULL, "chunk 1 unreadable (cut 1)");
    CHK(lch_is_ghost(lcw_space[a0].hdr), "node A ghost");

    /* ── verify cut 2+3: rewind ghosted + ref null ── */
    printf("\n▶ Cut 2+3: rewind ghost + ref null\n");
    CHK(rb.slots[0].ghosted == 1,  "rewind slot 0 ghosted");
    CHK(rb.slots[0].enc == 0,      "rewind slot 0 ref = null");
    CHK(rb.slots[0].valid == 0,    "rewind slot 0 invalid");
    CHK(rb.slots[1].ghosted == 1,  "rewind slot 1 ghosted");
    CHK(rb.slots[1].enc == 0,      "rewind slot 1 ref = null");
    CHK(rb.slots[2].ghosted == 1,  "rewind slot 2 ghosted");
    CHK(rb.slots[2].enc == 0,      "rewind slot 2 ref = null");

    /* ── verify slots still occupied (append-only) ── */
    printf("\n▶ Append-only: slots still occupied\n");
    CHK(lcw_space[a0].occupied == 1, "addr_a slot occupied");
    CHK(lcw_space[b0].occupied == 1, "addr_b slot occupied");

    /* ── verify RGB audit trail ── */
    printf("\n▶ RGB audit trail\n");
    CHK(lch_r(lcw_space[a0].hdr) > 0 ||
        lch_g(lcw_space[a0].hdr) > 0 ||
        lch_b(lcw_space[a0].hdr) > 0, "RGB trail in ghost node");

    /* ── seed still readable (rewind from seed) ── */
    printf("\n▶ Seed rewind still available\n");
    CHK(lcfs_rewind_seed(fd, 0) == 0x1111, "seed[0] intact");
    CHK(lcfs_rewind_seed(fd, 3) == 0x4444, "seed[3] intact");

    /* ── full audit ── */
    printf("\n▶ Full audit after delete\n");
    LCDeleteAudit audit = lcfs_audit(fd, &rb);
    lcfs_audit_print(&audit);
    CHK(audit.ghost_nodes    == 4, "4 ghost nodes");
    CHK(audit.occupied_slots == 4, "4 slots still occupied");
    CHK(audit.rewind_ghosts  >= 2, "rewind entries ghosted");
    CHK(audit.rgb_trail_intact,    "RGB trail intact");

    printf("\n══════════════════════════════════════════════════\n");
    printf("  PASS: %d   FAIL: %d\n", pass, fail);
    if (!fail) printf("✅ All PASS — triple-cut verified\n");
    else       printf("❌ %d FAIL\n", fail);
    return fail ? 1 : 0;
}
#endif
