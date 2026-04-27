/*
 * geo_cube_file_store.h — FGLS Cube File Store (Phase 5)
 * ═══════════════════════════════════════════════════════
 *
 * Format (4,896B = 34×144, ds=27→9 ✅):
 *
 *   [Header   32B ] magic/ver/mode/crc32/reserved_mask/dispatch_id
 *   [Meta    252B ] 9 active × 28B (seed + master_core + checksum)
 *   [Payload 4608B] 12×6×64B flat — reserved slots zero-filled
 *   ─────────────────────────────────────────────────────────────
 *   overhead = 288B = 2×144 ds=18→9 ✅
 *   total    = 4896B = 34×144       ✅
 *
 * Reconstruction: deterministic from seed (default mode)
 * Reserved slots: structural silence — GPU stride fixed ✅
 *
 * No malloc. No float. No heap.
 * ═══════════════════════════════════════════════════════
 */

#ifndef GEO_CUBE_FILE_STORE_H
#define GEO_CUBE_FILE_STORE_H

#include <stdint.h>
#include <string.h>
#include "geo_giant_array.h"

/* ── Constants ───────────────────────────────────────────────── */
#define GCFS_MAGIC              0x46474C53u   /* "FGLS" LE         */
#define GCFS_VERSION            0x01u
#define GCFS_MODE_DETERMINISTIC 0x00u
#define GCFS_MODE_SNAPSHOT      0x01u

#define GCFS_HEADER_BYTES       36u
#define GCFS_ACTIVE_COSETS       9u
#define GCFS_META_REC_BYTES     28u
#define GCFS_META_BYTES         (GCFS_ACTIVE_COSETS * GCFS_META_REC_BYTES)  /* 252 */
#define GCFS_PAYLOAD_BYTES      APEX_DISPATCH_BYTES                         /* 4608 */
#define GCFS_OVERHEAD_BYTES     (GCFS_HEADER_BYTES + GCFS_META_BYTES)  /* 288 */
#define GCFS_TOTAL_BYTES        (GCFS_HEADER_BYTES + GCFS_META_BYTES + GCFS_PAYLOAD_BYTES)
                                                                            /* 4896 */
/* ── Compile-time assertions ─────────────────────────────────── */
typedef char _gcfs_hdr_bytes_assert  [(GCFS_HEADER_BYTES   ==   36u) ? 1:-1];
typedef char _gcfs_meta_bytes_assert [(GCFS_META_BYTES     ==  252u) ? 1:-1];
typedef char _gcfs_overhead_assert   [(GCFS_OVERHEAD_BYTES ==  288u) ? 1:-1];
typedef char _gcfs_total_assert      [(GCFS_TOTAL_BYTES    == 4896u) ? 1:-1];

/* ── CubeFileHeader: 32B ─────────────────────────────────────── */
typedef struct {
    uint32_t magic;           /* GCFS_MAGIC "FGLS"    @0           */
    uint8_t  version;         /* GCFS_VERSION         @4           */
    uint8_t  mode;            /* GCFS_MODE_*          @5           */
    uint8_t  coset_count;     /* 12 (9 active+3 rsv)  @6           */
    uint8_t  active_count;    /* 9                    @7           */
    uint32_t crc32;           /* CRC32[meta+payload]  @8           */
    uint32_t reserved_mask;   /* bitmask cosets 9-11  @12          */
    uint32_t dispatch_id;     /* monotonic counter    @16          */
    uint8_t  _pad[16];        /* future              @20..31       */
} CubeFileHeader;             /* 32B ✅                            */

typedef char _gcfs_hdr_size_assert[(sizeof(CubeFileHeader) == 36u) ? 1:-1];

/* ── CosetMetaRec: 28B per active coset ─────────────────────── */
typedef struct {
    uint32_t seed_hi;         /* seed >> 32           @0           */
    uint32_t seed_lo;         /* seed & 0xFFFFFFFF    @4           */
    uint32_t master_hi;       /* master_core >> 32    @8           */
    uint32_t master_lo;       /* master_core lo       @12          */
    uint8_t  coset_id;        /* 0..11               @16           */
    uint8_t  _pad[3];         /*                     @17           */
    uint32_t checksum;        /* XOR fold verify     @20           */
    uint32_t _reserved;       /* future              @24           */
} CosetMetaRec;               /* 28B ✅                            */

typedef char _gcfs_rec_size_assert[(sizeof(CosetMetaRec) == 28u) ? 1:-1];

/* ── CubeFileStore: full in-memory layout ────────────────────── */
typedef struct {
    CubeFileHeader hdr;
    CosetMetaRec   meta[GCFS_ACTIVE_COSETS];
    uint8_t        payload[GCFS_PAYLOAD_BYTES];
} CubeFileStore;

/* ── u64 pack/unpack helpers ─────────────────────────────────── */
static inline void     _gcfs_pack(uint32_t *hi, uint32_t *lo, uint64_t v)
    { *hi=(uint32_t)(v>>32); *lo=(uint32_t)(v&0xFFFFFFFFu); }
static inline uint64_t _gcfs_unpack(uint32_t hi, uint32_t lo)
    { return ((uint64_t)hi<<32)|lo; }

/* ── CRC32 (portable, no table) ──────────────────────────────── */
static inline uint32_t gcfs_crc32(const uint8_t *buf, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (uint8_t b = 0; b < 8u; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
    }
    return ~crc;
}

/* ── gcfs_serialize: GiantArray → CubeFileStore ─────────────── */
static inline void gcfs_serialize(CubeFileStore    *fs,
                                   const GiantArray *ga,
                                   uint8_t           mode,
                                   uint32_t          reserved_mask)
{
    /* payload: flat-pack, reserved = zero */
    memset(fs->payload, 0, GCFS_PAYLOAD_BYTES);
    for (uint8_t c = 0; c < APEX_COSET_COUNT; c++) {
        if (reserved_mask & (1u << c)) continue;
        for (uint8_t f = 0; f < APEX_FRUSTUM_MOUNT; f++) {
            uint32_t off = apex_dispatch_offset(c, f);
            memcpy(fs->payload + off, &ga->cubes[c].faces[f], APEX_SLOT_BYTES);
            memset(fs->payload + off + 60u, 0, 4u);  /* force _pad=0 — kernel verify_ok writes here */
        }
    }

    /* per-coset meta (active only) */
    uint8_t mi = 0u;
    for (uint8_t c = 0; c < APEX_COSET_COUNT && mi < GCFS_ACTIVE_COSETS; c++) {
        if (reserved_mask & (1u << c)) continue;
        CosetMetaRec *r = &fs->meta[mi++];
        _gcfs_pack(&r->seed_hi,   &r->seed_lo,   ga->cubes[c].seed);
        _gcfs_pack(&r->master_hi, &r->master_lo,
                   (uint64_t)ga->cubes[c].master_core_lo);
        r->coset_id  = c;
        r->checksum  = ga->cubes[c].faces[0].checksum; /* face[0] anchor */
        r->_reserved = 0u;
        r->_pad[0] = r->_pad[1] = r->_pad[2] = 0u;
    }

    /* header */
    CubeFileHeader *h = &fs->hdr;
    h->magic         = GCFS_MAGIC;
    h->version       = GCFS_VERSION;
    h->mode          = mode;
    h->coset_count   = APEX_COSET_COUNT;
    h->active_count  = GCFS_ACTIVE_COSETS;
    h->reserved_mask = reserved_mask;
    h->dispatch_id   = ga->dispatch_id;
    memset(h->_pad, 0, sizeof(h->_pad));

    /* crc32: chain over meta then payload */
    uint32_t crc = gcfs_crc32((const uint8_t *)fs->meta, GCFS_META_BYTES);
    crc ^= gcfs_crc32(fs->payload, GCFS_PAYLOAD_BYTES);
    h->crc32 = crc;
}

/* ── gcfs_write: CubeFileStore → 4896B flat buffer ──────────── */
static inline void gcfs_write(const CubeFileStore *fs,
                               uint8_t out[GCFS_TOTAL_BYTES])
{
    memcpy(out,                                        &fs->hdr,     GCFS_HEADER_BYTES);
    memcpy(out + GCFS_HEADER_BYTES,                     fs->meta,    GCFS_META_BYTES);
    memcpy(out + GCFS_HEADER_BYTES + GCFS_META_BYTES,   fs->payload, GCFS_PAYLOAD_BYTES);
}

/* ── gcfs_read: 4896B flat buffer → CubeFileStore ───────────── */
/* Returns: 0=ok  -1=bad magic  -2=crc mismatch                  */
static inline int gcfs_read(CubeFileStore *fs,
                              const uint8_t  in[GCFS_TOTAL_BYTES])
{
    memcpy(&fs->hdr,    in,                                        GCFS_HEADER_BYTES);
    memcpy( fs->meta,   in + GCFS_HEADER_BYTES,                    GCFS_META_BYTES);
    memcpy( fs->payload,in + GCFS_HEADER_BYTES + GCFS_META_BYTES,  GCFS_PAYLOAD_BYTES);

    if (fs->hdr.magic != GCFS_MAGIC) return -1;

    uint32_t crc = gcfs_crc32((const uint8_t *)fs->meta, GCFS_META_BYTES);
    crc ^= gcfs_crc32(fs->payload, GCFS_PAYLOAD_BYTES);
    if (crc != fs->hdr.crc32) return -2;

    return 0;
}

/* ── gcfs_reconstruct: CubeFileStore → GiantArray ───────────── */
/* Returns: 0=ok  -1=master_core mismatch (corrupt)              */
static inline int gcfs_reconstruct(GiantArray          *ga,
                                    const CubeFileStore *fs)
{
    ga->active_mask = 0u;
    ga->dispatch_id = fs->hdr.dispatch_id;

    uint8_t mi = 0u;
    for (uint8_t c = 0; c < APEX_COSET_COUNT; c++) {
        if (fs->hdr.reserved_mask & (1u << c)) {
            memset(&ga->cubes[c], 0, sizeof(GiantCube));
            continue;
        }
        if (mi >= GCFS_ACTIVE_COSETS) continue;
        const CosetMetaRec *r = &fs->meta[mi++];

        uint64_t seed = _gcfs_unpack(r->seed_hi, r->seed_lo);
        giant_cube_init(&ga->cubes[c], seed, r->coset_id);
        ga->active_mask |= (1u << c);

        uint64_t stored_master = _gcfs_unpack(r->master_hi, r->master_lo);
        if ((uint64_t)ga->cubes[c].master_core_lo != stored_master) return -1;
    }
    return 0;
}

/* ── gcfs_payload_ptr: direct pointer into payload ───────────── */
static inline const uint8_t *gcfs_payload_ptr(const CubeFileStore *fs,
                                                uint8_t coset, uint8_t face)
{
    return fs->payload + apex_dispatch_offset(coset, face);
}

#endif /* GEO_CUBE_FILE_STORE_H */
