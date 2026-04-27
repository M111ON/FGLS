/*
 * lc_fs.h — LetterCube Filesystem / API Integration Layer
 * ═══════════════════════════════════════════════════════════════
 *
 * Bridges lc_wire.h (LC routing) ↔ geo_cube_file_store (4,896B chunks)
 *
 * Responsibilities:
 *   - lcfs_open   : map file → LC node chain (pair per 4,896B chunk)
 *   - lcfs_read   : route through LC pipeline → retrieve chunk
 *   - lcfs_delete : ghost all LC nodes in chain → open circuit
 *   - lcfs_rewind : traverse fold stack → reconstruct from seed
 *
 * Pipeline:
 *   file → chunk[] → LC pair per chunk → lc_wire route
 *   ghost delete → node state=0, pair state=0 → path amnesia
 *   content stays in chunk store untouched (append-only)
 *
 * No malloc. No float. No heap.
 * ═══════════════════════════════════════════════════════════════
 */

#ifndef LC_FS_H
#define LC_FS_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "lc_wire.h"

/* ══════════════════════════════════════════════════════
   CHUNK CONSTANTS (from geo_cube_file_store)
   ══════════════════════════════════════════════════════ */
#define LCFS_CHUNK_BYTES     4896u   /* GCFS_TOTAL_BYTES */
#define LCFS_MAX_CHUNKS      64u     /* max chunks per file handle */
#define LCFS_MAX_FILES       32u     /* max open file handles */
#define LCFS_SEED_BYTES      8u      /* seed per chunk meta */

/* ══════════════════════════════════════════════════════
   CHUNK DESCRIPTOR
   One LC node pair per chunk
   node A = chunk head (positive, reader side)
   node B = chunk tail (negative, writer side)
   ══════════════════════════════════════════════════════ */
typedef struct {
    uint32_t  addr_a;        /* LC node A address (reader) */
    uint32_t  addr_b;        /* LC node B address (writer) */
    uint32_t  chunk_offset;  /* byte offset in content store */
    uint32_t  chunk_size;    /* actual bytes in this chunk */
    uint64_t  seed;          /* deterministic reconstruct seed */
    uint8_t   ghosted;       /* 1 = deleted */
} LCFSChunk;

/* ══════════════════════════════════════════════════════
   FILE HANDLE
   ══════════════════════════════════════════════════════ */
#define LCFS_NAME_LEN  32u

typedef struct {
    char       name[LCFS_NAME_LEN];
    uint32_t   chunk_count;
    uint32_t   total_bytes;
    uint8_t    level;         /* LC_LEVEL_0..3 */
    uint8_t    open;          /* 1 = active handle */
    uint8_t    _pad[2];
    LCFSChunk  chunks[LCFS_MAX_CHUNKS];
    LCTraversal trav;         /* fold-aware traversal */
} LCFSFile;

/* ══════════════════════════════════════════════════════
   FILE STORE (global, no malloc)
   ══════════════════════════════════════════════════════ */
static LCFSFile lcfs_files[LCFS_MAX_FILES];
static uint32_t lcfs_next_addr = 1000u;  /* address allocator */

static inline void lcfs_init(void) {
    lcw_init();
    memset(lcfs_files, 0, sizeof(lcfs_files));
    lcfs_next_addr = 1000u;
}

/* ── allocate two LC addresses for a chunk pair ── */
static inline uint32_t lcfs_alloc_addr(void) {
    uint32_t a = lcfs_next_addr;
    lcfs_next_addr += 2u;
    if (lcfs_next_addr >= LCW_MAIN_SPACE)
        lcfs_next_addr = 1000u;  /* wrap — production: handle overflow */
    return a;
}

/* ── find free file slot ── */
static inline int lcfs_find_slot(void) {
    for (int i = 0; i < (int)LCFS_MAX_FILES; i++)
        if (!lcfs_files[i].open) return i;
    return -1;
}

/* ══════════════════════════════════════════════════════
   lcfs_open — register file as LC node chain
   chunk_sizes[] = size of each chunk (sum = total file size)
   seeds[]       = deterministic seed per chunk
   ══════════════════════════════════════════════════════ */
static inline int lcfs_open(const char     *name,
                              uint32_t        chunk_count,
                              const uint32_t *chunk_sizes,
                              const uint64_t *seeds,
                              uint8_t         level) {
    if (chunk_count > LCFS_MAX_CHUNKS) return -1;
    int slot = lcfs_find_slot();
    if (slot < 0) return -2;

    LCFSFile *f = &lcfs_files[slot];
    memset(f, 0, sizeof(LCFSFile));
    snprintf(f->name, LCFS_NAME_LEN, "%s", name);
    f->chunk_count = chunk_count;
    f->level       = level;
    f->open        = 1;
    lcw_trav_init(&f->trav, lcfs_next_addr, level);

    uint32_t offset = 0;
    for (uint32_t i = 0; i < chunk_count; i++) {
        uint32_t a = lcfs_alloc_addr();
        uint32_t b = a + 1u;

        /* derive RGB9 from chunk index — spread across complement space */
        uint8_t r = (uint8_t)((i * 3u + 1u) % 8u);
        uint8_t g = (uint8_t)((i * 5u + 2u) % 8u);
        uint8_t bl= (uint8_t)((i * 7u + 4u) % 8u);
        uint16_t ang = (uint16_t)((i * 23u) % LCH_ANG_STEPS);

        LCHdr hA = lch_pack(0, 64, r,     g,     bl,
                             level, ang, (uint8_t)(i % 4));
        LCHdr hB = lch_pack(1, 64, 7u-r,  7u-g,  7u-bl,
                             level, (uint16_t)((ang + LCH_ANG_HALF) % LCH_ANG_STEPS),
                             (uint8_t)(i % 4));

        lcw_insert(a, b, hA, "R");
        lcw_insert(b, a, hB, "W");

        f->chunks[i].addr_a       = a;
        f->chunks[i].addr_b       = b;
        f->chunks[i].chunk_offset = offset;
        f->chunks[i].chunk_size   = chunk_sizes[i];
        f->chunks[i].seed         = seeds ? seeds[i] : (uint64_t)i * 0xdeadbeef;
        f->chunks[i].ghosted      = 0;

        offset += chunk_sizes[i];
        f->total_bytes += chunk_sizes[i];
    }

    return slot;
}

/* ══════════════════════════════════════════════════════
   lcfs_read — route through LC pipeline for chunk i
   returns: chunk descriptor pointer or NULL if blocked
   ══════════════════════════════════════════════════════ */
static inline const LCFSChunk *lcfs_read(int fslot, uint32_t chunk_idx,
                                           const LCPalette *pal) {
    if (fslot < 0 || fslot >= (int)LCFS_MAX_FILES) return NULL;
    LCFSFile *f = &lcfs_files[fslot];
    if (!f->open || chunk_idx >= f->chunk_count) return NULL;

    LCFSChunk *c = &f->chunks[chunk_idx];
    if (c->ghosted) return NULL;   /* already deleted */

    LCWRouteOut out = lcw_route(c->addr_a, pal, &f->trav);

    switch (out.result) {
        case LCW_RESULT_WARP:
        case LCW_RESULT_ROUTE:
            return c;   /* readable */
        case LCW_RESULT_GHOST:
        case LCW_RESULT_GROUND:
        case LCW_RESULT_COLLISION:
        default:
            return NULL;   /* blocked → ground lane */
    }
}

/* ══════════════════════════════════════════════════════
   lcfs_delete — ghost all chunks in file
   file structure stays, content unreachable
   ══════════════════════════════════════════════════════ */
static inline int lcfs_delete(int fslot) {
    if (fslot < 0 || fslot >= (int)LCFS_MAX_FILES) return -1;
    LCFSFile *f = &lcfs_files[fslot];
    if (!f->open) return -2;

    for (uint32_t i = 0; i < f->chunk_count; i++) {
        lcw_ghost(f->chunks[i].addr_a);
        f->chunks[i].ghosted = 1;
    }
    /* keep f->open = 1 — handle still exists, just all chunks ghosted */
    return 0;
}

/* ══════════════════════════════════════════════════════
   lcfs_close — release file handle
   ══════════════════════════════════════════════════════ */
static inline int lcfs_close(int fslot) {
    if (fslot < 0 || fslot >= (int)LCFS_MAX_FILES) return -1;
    lcfs_files[fslot].open = 0;
    return 0;
}

/* ══════════════════════════════════════════════════════
   lcfs_rewind — reconstruct from seed without content
   returns seed for chunk i (caller uses to regenerate)
   ══════════════════════════════════════════════════════ */
static inline uint64_t lcfs_rewind_seed(int fslot, uint32_t chunk_idx) {
    if (fslot < 0 || fslot >= (int)LCFS_MAX_FILES) return 0;
    LCFSFile *f = &lcfs_files[fslot];
    if (!f->open || chunk_idx >= f->chunk_count) return 0;
    return f->chunks[chunk_idx].seed;
}

/* ══════════════════════════════════════════════════════
   lcfs_stat — print file info
   ══════════════════════════════════════════════════════ */
static inline void lcfs_stat(int fslot) {
    if (fslot < 0 || fslot >= (int)LCFS_MAX_FILES) return;
    LCFSFile *f = &lcfs_files[fslot];
    if (!f->open) { printf("  [%d] closed\n", fslot); return; }

    uint32_t ghosted = 0;
    for (uint32_t i = 0; i < f->chunk_count; i++)
        ghosted += f->chunks[i].ghosted;

    printf("  [%d] %-20s  chunks=%u  ghosted=%u  bytes=%u  L%d\n",
           fslot, f->name, f->chunk_count, ghosted,
           f->total_bytes, f->level);
}

#endif /* LC_FS_H */

/* ══════════════════════════════════════════════════════
   SELF-TEST
   gcc -O2 -DTEST_LC_FS -o lc_fs_test -x c lc_fs.h
   ══════════════════════════════════════════════════════ */
#ifdef TEST_LC_FS
int main(void) {
    int pass=0, fail=0;
#define CHK(cond,msg) do{ if(cond){printf("  ✓ %s\n",msg);pass++;} \
                          else{printf("  ✗ FAIL: %s\n",msg);fail++;} }while(0)

    printf("╔══════════════════════════════════════════════╗\n");
    printf("║  lc_fs.h — Integration Layer Test           ║\n");
    printf("╚══════════════════════════════════════════════╝\n\n");

    lcfs_init();

    /* ── T1: open file with 3 chunks ── */
    printf("▶ T1: open file — 3 chunks\n");
    uint32_t sizes[3] = {4896, 4896, 2048};
    uint64_t seeds[3] = {0xDEAD, 0xBEEF, 0xCAFE};
    int fd = lcfs_open("test.bin", 3, sizes, seeds, LC_LEVEL_1);
    printf("  fd=%d\n", fd);
    CHK(fd >= 0, "open returns valid fd");
    lcfs_stat(fd);
    CHK(lcfs_files[fd].chunk_count == 3,   "chunk_count=3");
    CHK(lcfs_files[fd].total_bytes == 11840, "total_bytes=11840");

    /* ── T2: read chunks (no palette restriction) ── */
    printf("\n▶ T2: read chunks\n");
    LCPalette pal = {0};
    /* set all face bits to pass */
    for (int f=0; f<4; f++) pal.mask[f] = ~0ull;
    const LCFSChunk *c0 = lcfs_read(fd, 0, &pal);
    const LCFSChunk *c1 = lcfs_read(fd, 1, &pal);
    const LCFSChunk *c2 = lcfs_read(fd, 2, &pal);
    CHK(c0 != NULL, "read chunk 0 OK");
    CHK(c1 != NULL, "read chunk 1 OK");
    CHK(c2 != NULL, "read chunk 2 OK");
    CHK(c0->seed == 0xDEAD, "chunk 0 seed correct");
    CHK(c2->chunk_size == 2048, "chunk 2 size=2048");

    /* ── T3: delete file → ghost all chunks ── */
    printf("\n▶ T3: delete file\n");
    int r = lcfs_delete(fd);
    CHK(r == 0, "delete returns 0");
    lcfs_stat(fd);

    /* all chunks now ghosted */
    const LCFSChunk *gc0 = lcfs_read(fd, 0, &pal);
    const LCFSChunk *gc1 = lcfs_read(fd, 1, &pal);
    CHK(gc0 == NULL, "read after delete → NULL (path amnesia)");
    CHK(gc1 == NULL, "read chunk 1 after delete → NULL");

    /* LC nodes still occupied */
    uint32_t a0 = lcfs_files[fd].chunks[0].addr_a;
    CHK(lcw_space[a0].occupied == 1,       "slot still occupied (append-only)");
    CHK(lch_is_ghost(lcw_space[a0].hdr),   "node is ghost");
    CHK(lch_r(lcw_space[a0].hdr) > 0 ||
        lch_g(lcw_space[a0].hdr) > 0,      "RGB audit trail preserved");

    /* ── T4: rewind seed still available ── */
    printf("\n▶ T4: rewind seed after delete\n");
    uint64_t s0 = lcfs_rewind_seed(fd, 0);
    uint64_t s2 = lcfs_rewind_seed(fd, 2);
    printf("  seed[0]=0x%llX  seed[2]=0x%llX\n",
           (unsigned long long)s0, (unsigned long long)s2);
    CHK(s0 == 0xDEAD, "seed[0] still readable after ghost");
    CHK(s2 == 0xCAFE, "seed[2] still readable after ghost");

    /* ── T5: palette block → NULL ── */
    printf("\n▶ T5: palette filter blocks read\n");
    int fd2 = lcfs_open("test2.bin", 2, sizes, seeds, LC_LEVEL_1);
    LCPalette empty = {0};  /* no bits set */
    const LCFSChunk *blocked = lcfs_read(fd2, 0, &empty);
    CHK(blocked == NULL, "empty palette → read blocked (ground)");

    /* ── T6: close ── */
    printf("\n▶ T6: close\n");
    CHK(lcfs_close(fd)  == 0, "close fd OK");
    CHK(lcfs_close(fd2) == 0, "close fd2 OK");
    CHK(lcfs_files[fd].open == 0, "fd closed");

    /* ── T7: invalid ops ── */
    printf("\n▶ T7: invalid ops\n");
    CHK(lcfs_read(-1, 0, &pal)   == NULL, "read(-1) = NULL");
    CHK(lcfs_delete(-1)          == -1,   "delete(-1) = -1");
    CHK(lcfs_rewind_seed(-1, 0)  == 0,    "seed(-1) = 0");

    /* ── T8: sizeof ── */
    printf("\n▶ T8: sizeof\n");
    printf("  LCFSChunk = %zu bytes\n", sizeof(LCFSChunk));
    printf("  LCFSFile  = %zu bytes\n", sizeof(LCFSFile));
    CHK(sizeof(LCFSChunk) <= 40, "LCFSChunk <= 40 bytes");

    printf("\n══════════════════════════════════════════════\n");
    printf("  PASS: %d   FAIL: %d\n", pass, fail);
    if (!fail) printf("✅ All PASS\n");
    else       printf("❌ %d FAIL\n", fail);
    return fail ? 1 : 0;
}
#endif
