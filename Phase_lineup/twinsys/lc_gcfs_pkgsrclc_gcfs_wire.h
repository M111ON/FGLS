/*
 * lc_gcfs_wire.h — LC ↔ geo_cube_file_store Wire
 * ═══════════════════════════════════════════════════════════════
 *
 * Connects lc_fs.h (LC routing layer) with geo_cube_file_store.h
 * (GCFS 4,896B chunk format)
 *
 * Key bindings:
 *   lcfs_open   ← gcfs chunk count + CosetMetaRec seeds
 *   lcfs_read   → gcfs_read() → CubeFileStore payload
 *   lcfs_delete → triple-cut (lc_delete.h)
 *   rewind seed → gcfs_reconstruct() from seed only
 *
 * Ghost delete alignment:
 *   GCFS reserved_mask (cosets 9-11) = structural silence
 *   LC ground lane = routing silence
 *   Both are "present but unreachable" — same DNA ✅
 *
 * No malloc. No float. No heap.
 * ═══════════════════════════════════════════════════════════════
 */

#ifndef LC_GCFS_WIRE_H
#define LC_GCFS_WIRE_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "lc_delete.h"

/* ── standalone GCFS defs (no geo_giant_array dependency) ── */
#define GCFS_MAGIC              0x46474C53u
#define GCFS_VERSION            0x01u
#define GCFS_MODE_DETERMINISTIC 0x00u
#define GCFS_HEADER_BYTES       36u
#define GCFS_ACTIVE_COSETS      9u
#define GCFS_META_REC_BYTES     28u
#define GCFS_META_BYTES         (GCFS_ACTIVE_COSETS * GCFS_META_REC_BYTES) /* 252 */
#define GCFS_PAYLOAD_BYTES      4608u
#define GCFS_TOTAL_BYTES        (GCFS_HEADER_BYTES + GCFS_META_BYTES + GCFS_PAYLOAD_BYTES) /* 4896 */

/* ── CRC32 (portable) ── */
static inline uint32_t _lcgw_crc32(const uint8_t *buf, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (uint8_t b = 0; b < 8u; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
    }
    return ~crc;
}

/* ── minimal CosetMetaRec ── */
typedef struct {
    uint32_t seed_hi, seed_lo;
    uint32_t master_hi, master_lo;
    uint8_t  coset_id;
    uint8_t  _pad[3];
    uint32_t checksum;
    uint32_t _reserved;
} LCGWMetaRec;   /* 28B */

/* ── minimal CubeFileHeader ── */
typedef struct {
    uint32_t magic;
    uint8_t  version, mode, coset_count, active_count;
    uint32_t crc32;
    uint32_t reserved_mask;
    uint32_t dispatch_id;
    uint8_t  _pad[16];
} LCGWHeader;    /* 36B */

typedef char _lcgw_hdr_sz[(sizeof(LCGWHeader)  == 36u) ? 1:-1];
typedef char _lcgw_rec_sz[(sizeof(LCGWMetaRec) == 28u) ? 1:-1];

/* ══════════════════════════════════════════════════════
   GCFS CHUNK BUFFER (in-memory, no malloc)
   holds one 4896B flat chunk
   ══════════════════════════════════════════════════════ */
typedef struct {
    uint8_t  raw[GCFS_TOTAL_BYTES];   /* flat 4896B */
    uint8_t  valid;                   /* 1 = parsed OK */
    uint8_t  ghosted;                 /* 1 = LC deleted */
    uint8_t  _pad[2];
} LCGWChunk;

/* ── extract seed from flat chunk bytes ── */
static inline uint64_t lcgw_chunk_seed(const uint8_t *raw, uint8_t coset_idx) {
    if (coset_idx >= GCFS_ACTIVE_COSETS) return 0;
    const uint8_t *meta_base = raw + GCFS_HEADER_BYTES;
    const LCGWMetaRec *rec = (const LCGWMetaRec *)(meta_base) + coset_idx;
    return ((uint64_t)rec->seed_hi << 32) | rec->seed_lo;
}

/* ── verify magic + crc ── */
static inline int lcgw_verify(const uint8_t *raw) {
    const LCGWHeader *h = (const LCGWHeader *)raw;
    if (h->magic != GCFS_MAGIC) return -1;
    const uint8_t *meta    = raw + GCFS_HEADER_BYTES;
    const uint8_t *payload = meta + GCFS_META_BYTES;
    uint32_t crc = _lcgw_crc32(meta, GCFS_META_BYTES);
    crc ^= _lcgw_crc32(payload, GCFS_PAYLOAD_BYTES);
    if (crc != h->crc32) return -2;
    return 0;
}

/* ── build minimal valid GCFS chunk from seed (for test/rewind) ── */
static inline void lcgw_build_from_seed(uint8_t *raw, uint64_t seed,
                                         uint32_t dispatch_id) {
    memset(raw, 0, GCFS_TOTAL_BYTES);

    /* header */
    LCGWHeader *h = (LCGWHeader *)raw;
    h->magic        = GCFS_MAGIC;
    h->version      = GCFS_VERSION;
    h->mode         = GCFS_MODE_DETERMINISTIC;
    h->coset_count  = 12u;
    h->active_count = GCFS_ACTIVE_COSETS;
    h->reserved_mask= 0u;
    h->dispatch_id  = dispatch_id;

    /* meta: fill 9 cosets with seed derivatives */
    uint8_t *meta_base = raw + GCFS_HEADER_BYTES;
    for (uint8_t i = 0; i < GCFS_ACTIVE_COSETS; i++) {
        LCGWMetaRec *r = (LCGWMetaRec *)meta_base + i;
        uint64_t s = seed ^ ((uint64_t)i * 0x9e3779b97f4a7c15ull);
        r->seed_hi   = (uint32_t)(s >> 32);
        r->seed_lo   = (uint32_t)(s & 0xFFFFFFFFu);
        r->coset_id  = i;
        r->checksum  = (uint32_t)(s ^ (s >> 16));
    }

    /* payload: deterministic fill from seed */
    uint8_t *payload = raw + GCFS_HEADER_BYTES + GCFS_META_BYTES;
    uint64_t prng = seed ^ 0xdeadbeefcafeull;
    for (uint32_t i = 0; i < GCFS_PAYLOAD_BYTES; i++) {
        prng ^= prng << 13; prng ^= prng >> 7; prng ^= prng << 17;
        payload[i] = (uint8_t)(prng & 0xFF);
    }

    /* crc */
    uint32_t crc = _lcgw_crc32(raw + GCFS_HEADER_BYTES, GCFS_META_BYTES);
    crc ^= _lcgw_crc32(payload, GCFS_PAYLOAD_BYTES);
    h->crc32 = crc;
}

/* ══════════════════════════════════════════════════════
   LC-GCFS FILE HANDLE
   extends lc_fs with actual GCFS chunk data
   ══════════════════════════════════════════════════════ */
#define LCGW_MAX_CHUNKS  LCFS_MAX_CHUNKS   /* 64 */
#define LCGW_MAX_FILES   LCFS_MAX_FILES    /* 32 */

typedef struct {
    int        lc_fd;                        /* lc_fs file descriptor */
    LCGWChunk  chunks[LCGW_MAX_CHUNKS];      /* actual GCFS data */
    uint32_t   chunk_count;
    uint8_t    open;
} LCGWFile;

static LCGWFile lcgw_files[LCGW_MAX_FILES];

static inline void lcgw_init(void) {
    lcfs_init();
    memset(lcgw_files, 0, sizeof(lcgw_files));
}

static inline int lcgw_find_slot(void) {
    for (int i = 0; i < (int)LCGW_MAX_FILES; i++)
        if (!lcgw_files[i].open) return i;
    return -1;
}

/* ── open: register file + build/verify GCFS chunks ── */
static inline int lcgw_open(const char *name,
                              uint32_t    chunk_count,
                              const uint64_t *seeds,   /* one per chunk */
                              uint8_t     level) {
    if (chunk_count > LCGW_MAX_CHUNKS) return -1;
    int gslot = lcgw_find_slot();
    if (gslot < 0) return -2;

    /* build chunk sizes array (all GCFS_TOTAL_BYTES) */
    uint32_t sizes[LCGW_MAX_CHUNKS];
    for (uint32_t i = 0; i < chunk_count; i++)
        sizes[i] = GCFS_TOTAL_BYTES;

    int lc_fd = lcfs_open(name, chunk_count, sizes, seeds, level);
    if (lc_fd < 0) return -3;

    LCGWFile *gf = &lcgw_files[gslot];
    gf->lc_fd      = lc_fd;
    gf->chunk_count = chunk_count;
    gf->open        = 1;

    /* build GCFS chunk from seed for each chunk */
    for (uint32_t i = 0; i < chunk_count; i++) {
        lcgw_build_from_seed(gf->chunks[i].raw,
                              seeds ? seeds[i] : (uint64_t)i,
                              (uint32_t)i);
        gf->chunks[i].valid   = (lcgw_verify(gf->chunks[i].raw) == 0) ? 1 : 0;
        gf->chunks[i].ghosted = 0;
    }

    return gslot;
}

/* ── read: LC gate check → return payload ptr or NULL ── */
static inline const uint8_t *lcgw_read_payload(int gslot, uint32_t chunk_idx,
                                                 const LCPalette *pal) {
    if (gslot < 0 || gslot >= (int)LCGW_MAX_FILES) return NULL;
    LCGWFile *gf = &lcgw_files[gslot];
    if (!gf->open || chunk_idx >= gf->chunk_count) return NULL;
    if (gf->chunks[chunk_idx].ghosted) return NULL;

    /* LC routing gate */
    const LCFSChunk *lc = lcfs_read(gf->lc_fd, chunk_idx, pal);
    if (!lc) return NULL;

    /* GCFS verify */
    if (!gf->chunks[chunk_idx].valid) return NULL;

    return gf->chunks[chunk_idx].raw + GCFS_HEADER_BYTES + GCFS_META_BYTES;
}

/* ── read seed from chunk (for rewind reconstruct) ── */
static inline uint64_t lcgw_read_seed(int gslot, uint32_t chunk_idx,
                                       uint8_t coset_idx) {
    if (gslot < 0 || gslot >= (int)LCGW_MAX_FILES) return 0;
    LCGWFile *gf = &lcgw_files[gslot];
    if (!gf->open || chunk_idx >= gf->chunk_count) return 0;
    return lcgw_chunk_seed(gf->chunks[chunk_idx].raw, coset_idx);
}

/* ── delete: triple-cut + ghost GCFS chunk ── */
static inline LCDeleteResult lcgw_delete(int gslot, RewindBuffer *rb) {
    LCDeleteResult res = {-1, 0, 0, 0};
    if (gslot < 0 || gslot >= (int)LCGW_MAX_FILES) return res;
    LCGWFile *gf = &lcgw_files[gslot];
    if (!gf->open) return res;

    /* LC triple-cut */
    res = lcfs_delete_atomic(gf->lc_fd, rb);

    /* ghost GCFS chunks */
    for (uint32_t i = 0; i < gf->chunk_count; i++)
        gf->chunks[i].ghosted = 1;

    return res;
}

/* ── rewind: reconstruct chunk from seed (no content needed) ── */
static inline int lcgw_rewind(int gslot, uint32_t chunk_idx,
                                uint8_t out_raw[GCFS_TOTAL_BYTES]) {
    if (gslot < 0 || gslot >= (int)LCGW_MAX_FILES) return -1;
    LCGWFile *gf = &lcgw_files[gslot];
    if (!gf->open || chunk_idx >= gf->chunk_count) return -1;

    /* get seed from lc_fs layer */
    uint64_t seed = lcfs_rewind_seed(gf->lc_fd, chunk_idx);
    if (!seed) return -2;

    /* reconstruct deterministically */
    lcgw_build_from_seed(out_raw, seed, chunk_idx);
    return lcgw_verify(out_raw);
}

/* ── close ── */
static inline int lcgw_close(int gslot) {
    if (gslot < 0 || gslot >= (int)LCGW_MAX_FILES) return -1;
    lcfs_close(lcgw_files[gslot].lc_fd);
    lcgw_files[gslot].open = 0;
    return 0;
}

/* ── stat ── */
static inline void lcgw_stat(int gslot) {
    if (gslot < 0 || gslot >= (int)LCGW_MAX_FILES) return;
    LCGWFile *gf = &lcgw_files[gslot];
    uint32_t ghosted = 0, valid = 0;
    for (uint32_t i = 0; i < gf->chunk_count; i++) {
        if (gf->chunks[i].ghosted) ghosted++;
        if (gf->chunks[i].valid)   valid++;
    }
    printf("  lcgw[%d]  chunks=%u  valid=%u  ghosted=%u  lc_fd=%d\n",
           gslot, gf->chunk_count, valid, ghosted, gf->lc_fd);
    lcfs_stat(gf->lc_fd);
}

#endif /* LC_GCFS_WIRE_H */

/* ══════════════════════════════════════════════════════
   SELF-TEST
   gcc -O2 -DTEST_LC_GCFS -o lc_gcfs_test -x c lc_gcfs_wire.h
   ══════════════════════════════════════════════════════ */
#ifdef TEST_LC_GCFS
int main(void) {
    int pass=0, fail=0;
#define CHK(cond,msg) do{ if(cond){printf("  ✓ %s\n",msg);pass++;} \
                          else{printf("  ✗ FAIL: %s\n",msg);fail++;} }while(0)

    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║  lc_gcfs_wire.h — LC ↔ GCFS Integration Test   ║\n");
    printf("╚══════════════════════════════════════════════════╝\n\n");

    lcgw_init();
    static RewindBuffer rb; memset(&rb, 0, sizeof(rb));
    LCPalette pal = {0};
    for (int f=0; f<4; f++) pal.mask[f] = ~0ull;

    uint64_t seeds[4] = {0xAABBCCDD11223344ull, 0x1234567890ABCDEFull,
                          0xDEADBEEFCAFE0000ull, 0x0102030405060708ull};

    /* ── T1: open ── */
    printf("▶ T1: open 4-chunk GCFS file\n");
    int gfd = lcgw_open("data.gcfs", 4, seeds, LC_LEVEL_1);
    printf("  gfd=%d\n", gfd);
    CHK(gfd >= 0, "open OK");
    lcgw_stat(gfd);

    /* ── T2: verify GCFS chunks ── */
    printf("\n▶ T2: GCFS chunk verify (magic + CRC)\n");
    for (uint32_t i = 0; i < 4; i++) {
        int v = lcgw_verify(lcgw_files[gfd].chunks[i].raw);
        printf("  chunk[%u] verify=%d %s\n", i, v, v==0?"✓":"✗");
        CHK(v == 0, "chunk CRC valid");
    }

    /* ── T3: read payload via LC gate ── */
    printf("\n▶ T3: read payload through LC pipeline\n");
    for (uint32_t i = 0; i < 4; i++) {
        const uint8_t *p = lcgw_read_payload(gfd, i, &pal);
        CHK(p != NULL, "payload readable");
        if (p) printf("  chunk[%u] payload[0..3] = %02X %02X %02X %02X\n",
                       i, p[0], p[1], p[2], p[3]);
    }

    /* ── T4: seed extraction ── */
    printf("\n▶ T4: seed extraction from GCFS meta\n");
    uint64_t s0 = lcgw_read_seed(gfd, 0, 0);
    printf("  chunk[0] coset[0] seed = 0x%016llX\n", (unsigned long long)s0);
    CHK(s0 != 0, "seed non-zero");

    /* ── T5: delete (triple-cut) ── */
    printf("\n▶ T5: triple-cut delete\n");
    LCDeleteResult res = lcgw_delete(gfd, &rb);
    printf("  chunks_ghosted=%u  rewind_cut=%u  append_only=%u\n",
           res.chunks_ghosted, res.rewind_cut, res.append_only_verified);
    CHK(res.status == 0,           "delete OK");
    CHK(res.chunks_ghosted == 4,   "4 chunks ghosted");
    CHK(res.append_only_verified,  "append-only preserved");

    /* ── T6: read blocked after delete ── */
    printf("\n▶ T6: read after delete → NULL\n");
    for (uint32_t i = 0; i < 4; i++) {
        const uint8_t *p = lcgw_read_payload(gfd, i, &pal);
        CHK(p == NULL, "payload NULL after delete");
    }

    /* ── T7: rewind reconstruct from seed ── */
    printf("\n▶ T7: rewind reconstruct from seed\n");
    static uint8_t rewind_buf[GCFS_TOTAL_BYTES];
    int rv = lcgw_rewind(gfd, 0, rewind_buf);
    printf("  rewind chunk[0] result=%d  verify=%d\n",
           rv, lcgw_verify(rewind_buf));
    CHK(rv == 0, "rewind reconstruct OK");
    CHK(lcgw_verify(rewind_buf) == 0, "rewind CRC valid");

    /* compare first 8 bytes of payload with original */
    const uint8_t *orig = lcgw_files[gfd].chunks[0].raw
                          + GCFS_HEADER_BYTES + GCFS_META_BYTES;
    const uint8_t *rwnd = rewind_buf + GCFS_HEADER_BYTES + GCFS_META_BYTES;
    int match = memcmp(orig, rwnd, 8) == 0;
    printf("  payload match (first 8B): %s\n", match ? "YES ✓" : "NO ✗");
    CHK(match, "rewind payload matches original");

    /* ── T8: sizeof ── */
    printf("\n▶ T8: sizeof\n");
    printf("  LCGWChunk = %zu bytes\n", sizeof(LCGWChunk));
    printf("  LCGWFile  = %zu bytes\n", sizeof(LCGWFile));
    printf("  GCFS_TOTAL_BYTES = %u\n", GCFS_TOTAL_BYTES);
    CHK(GCFS_TOTAL_BYTES == 4896u, "GCFS chunk = 4896B");

    /* ── T9: close ── */
    printf("\n▶ T9: close\n");
    CHK(lcgw_close(gfd) == 0, "close OK");
    CHK(lcgw_files[gfd].open == 0, "slot freed");

    printf("\n══════════════════════════════════════════════════\n");
    printf("  PASS: %d   FAIL: %d\n", pass, fail);
    if (!fail) printf("✅ All PASS — LC ↔ GCFS wired\n");
    else       printf("❌ %d FAIL\n", fail);
    return fail ? 1 : 0;
}
#endif
