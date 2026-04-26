/*
 * pogls_m6.c — M6 P1: DeltaFabric Snapshot Serialize
 * ====================================================
 * Portable field-by-field binary format (little-endian, no padding leaks)
 * P2 alignment: lane_idx order preserved → GhostRef serialize can key on lane
 *
 * Format spec (locked M6):
 *   [4B] magic      = 0x504C4601  ("PLF\x01")
 *   [4B] version    = 1
 *   [4B] lane_count = PIPE_POOL (54)
 *   [4B] lane_cap   = LANE_CAP  (8)
 *   [8B] global_checksum  (XOR fold of all lane checksums)
 *   per lane[0..53]:
 *     [1B] count
 *     [7B] _pad (reserved, zeroed — P2 will use for GhostRef face_idx map)
 *     [8B × count] cores
 *     [8B] lane_checksum
 *   [4B] total_written
 *   [4B] total_collisions
 *   [4B] crc32  (simple XOR fold over all written bytes — lightweight integrity)
 *
 * Zero-heap: caller provides buffer. Returns bytes written / bytes consumed.
 * Error codes: < 0
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* ── primitives (M5 carry-forward) ──────────────────────────── */
static inline uint64_t mix64(uint64_t x) {
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27; x *= 0x94D049BB133111EBULL;
    x ^= x >> 31; return x;
}
static inline uint64_t slope(uint64_t core, uint8_t face) {
    return mix64(core ^ ((uint64_t)face * 0x9E3779B185EBCA87ULL));
}
static inline int is_complement(uint8_t a, uint8_t b) {
    return a < 6 && b < 6 && (a + 3) % 6 == b;
}
static inline int fibo_apex(uint64_t c0, uint64_t c1) {
    int pc = __builtin_popcountll(c0 ^ c1);
    return (pc >= 15 && pc <= 20) || (pc >= 44 && pc <= 49);
}
static inline uint64_t derive_child(uint64_t parent, uint64_t pat, uint8_t depth) {
    return mix64(parent ^ pat ^ ((uint64_t)depth * 0x9E3779B185EBCA87ULL));
}

/* ── constants ──────────────────────────────────────────────── */
#define PIPE_SLOTS   15
#define PIPE_POOL    54
#define LANE_CAP      8
#define SNAP_MAGIC   0x504C4601UL  /* "PLF\x01" */
#define SNAP_VERSION 1

/* ── structs (M5 carry-forward) ─────────────────────────────── */
typedef struct { uint64_t gen1; uint64_t gen2; } GeoSeed;
typedef struct {
    uint64_t core; uint8_t face; uint8_t depth;
    uint8_t occupied; uint8_t _pad;
} PipeSlot;
typedef struct {
    PipeSlot slots[PIPE_SLOTS];
    uint32_t tick;
    uint64_t seed_core;
} PipelineWire;
typedef struct {
    uint64_t cores[LANE_CAP];
    uint8_t  count;
    uint64_t checksum;
} DeltaLane;
typedef struct {
    DeltaLane lanes[PIPE_POOL];
    uint32_t  total_written;
    uint32_t  total_collisions;
} DeltaFabric;

/* ── M5 helpers ─────────────────────────────────────────────── */
static inline void fabric_init(DeltaFabric *df) {
    memset(df, 0, sizeof(DeltaFabric));
}
static inline uint8_t fabric_write(DeltaFabric *df, uint8_t pipe_slot_idx, uint64_t core) {
    uint8_t lane_idx = pipe_slot_idx % PIPE_POOL;
    DeltaLane *lane  = &df->lanes[lane_idx];
    if (lane->count >= LANE_CAP) { df->total_collisions++; return 0xFF; }
    lane->cores[lane->count++] = core;
    lane->checksum ^= core;
    df->total_written++;
    return lane_idx;
}
static inline uint64_t fabric_checksum(const DeltaFabric *df) {
    uint64_t acc = 0;
    for (int i = 0; i < PIPE_POOL; i++) acc ^= df->lanes[i].checksum;
    return acc;
}
static inline GeoSeed seed_init(uint64_t genesis) {
    GeoSeed s = { genesis, mix64(genesis) };
    for (int t = 0; t < 64; t++) {
        for (uint8_t a = 0; a < 6; a++)
            for (uint8_t b = a+1; b < 6; b++)
                if (!is_complement(a,b) && fibo_apex(slope(s.gen2,a), slope(s.gen2,b)))
                    return s;
        s.gen2 = mix64(s.gen2);
    }
    return s;
}
static inline void wire_init(PipelineWire *pw, GeoSeed s) {
    memset(pw, 0, sizeof(PipelineWire));
    pw->seed_core = s.gen2;
    pw->slots[0].core = s.gen2; pw->slots[0].face = 0;
    pw->slots[0].depth = 0; pw->slots[0].occupied = 1;
}
static inline uint8_t wire_node(PipelineWire *pw, uint64_t core, uint8_t face, uint8_t depth) {
    uint8_t idx = (uint8_t)(slope(core, face) % PIPE_SLOTS);
    PipeSlot *s = &pw->slots[idx];
    if (!s->occupied || s->depth > depth) {
        s->core = core; s->face = face; s->depth = depth; s->occupied = 1;
        pw->tick++;
        return idx;
    }
    return 0xFF;
}
static inline uint8_t wire_occupancy(const PipelineWire *pw) {
    uint8_t n = 0;
    for (int i = 0; i < PIPE_SLOTS; i++) n += pw->slots[i].occupied;
    return n;
}
static inline uint32_t scan_wire(PipelineWire *pw, GeoSeed s, uint32_t n) {
    uint32_t wired = 0;
    uint64_t cur = s.gen2;
    for (uint32_t i = 0; i < n && i < PIPE_SLOTS; i++) {
        uint8_t face = (uint8_t)(i % 6);
        uint64_t pat  = slope(cur, face) ^ slope(cur, (face+1)%6);
        uint64_t next = derive_child(cur, pat, (uint8_t)(i+1));
        uint8_t r = wire_node(pw, next, face, (uint8_t)(i % 4));
        if (r != 0xFF) { wired++; cur = next; }
    }
    return wired;
}
static inline uint32_t fabric_fold_wire(DeltaFabric *df, const PipelineWire *pw) {
    uint32_t written = 0;
    for (uint8_t i = 0; i < PIPE_SLOTS; i++) {
        if (!pw->slots[i].occupied) continue;
        if (fabric_write(df, i, pw->slots[i].core) != 0xFF) written++;
    }
    return written;
}

/* ══════════════════════════════════════════════════════════════
 * M6 P1 — Portable Serialize / Deserialize
 * ══════════════════════════════════════════════════════════════ */

/* ── write_u8/u32/u64: explicit little-endian, no struct padding ── */
static inline void w8(uint8_t **p, uint8_t v)  { **p = v; (*p)++; }
static inline void w32(uint8_t **p, uint32_t v) {
    (*p)[0]=(uint8_t)(v);      (*p)[1]=(uint8_t)(v>>8);
    (*p)[2]=(uint8_t)(v>>16);  (*p)[3]=(uint8_t)(v>>24);
    *p += 4;
}
static inline void w64(uint8_t **p, uint64_t v) {
    for (int i=0;i<8;i++) { (*p)[i]=(uint8_t)(v>>(i*8)); }
    *p += 8;
}
static inline uint8_t  r8(const uint8_t **p)  { return *(*p)++; }
static inline uint32_t r32(const uint8_t **p) {
    uint32_t v = (uint32_t)(*p)[0] | ((uint32_t)(*p)[1]<<8)
               | ((uint32_t)(*p)[2]<<16) | ((uint32_t)(*p)[3]<<24);
    *p += 4; return v;
}
static inline uint64_t r64(const uint8_t **p) {
    uint64_t v = 0;
    for (int i=0;i<8;i++) v |= ((uint64_t)(*p)[i])<<(i*8);
    *p += 8; return v;
}

/* ── crc32_xor: lightweight integrity (XOR fold, not CRC-32 poly) ── */
static uint32_t crc32_xor(const uint8_t *buf, size_t len) {
    uint32_t acc = 0;
    for (size_t i = 0; i < len; i += 4) {
        uint32_t chunk = 0;
        for (int j = 0; j < 4 && i+j < len; j++)
            chunk |= ((uint32_t)buf[i+j]) << (j*8);
        acc ^= chunk;
    }
    return acc;
}

/*
 * fabric_snapshot_size — exact byte count for buffer pre-alloc
 */
static inline size_t fabric_snapshot_size(void) {
    /* header: magic(4) + version(4) + lane_count(4) + lane_cap(4)
               + global_checksum(8) = 24B
     * per lane: count(1) + pad(7) + cores(8×LANE_CAP) + checksum(8)
               = 1 + 7 + 64 + 8 = 80B  × 54 lanes = 4320B
     * footer: total_written(4) + total_collisions(4) + crc32(4) = 12B
     * Total = 24 + 4320 + 12 = 4356B
     */
    return 24 + (size_t)PIPE_POOL * (1 + 7 + (size_t)LANE_CAP * 8 + 8) + 12;
}

/*
 * fabric_serialize — DeltaFabric → portable binary snapshot
 *
 * @df  : source fabric (read-only)
 * @buf : caller buffer, must be >= fabric_snapshot_size() bytes
 * @cap : buffer capacity
 * Returns: bytes written, or -1 on error
 *
 * P2 alignment note:
 *   pad[7] bytes per lane reserved for GhostRef face_idx bitmap
 *   (P2 will write face_idx presence flags here — format locked now)
 */
static int fabric_serialize(const DeltaFabric *df, uint8_t *buf, size_t cap) {
    size_t needed = fabric_snapshot_size();
    if (cap < needed) return -1;

    uint8_t *p = buf;

    /* header */
    w32(&p, SNAP_MAGIC);
    w32(&p, SNAP_VERSION);
    w32(&p, PIPE_POOL);
    w32(&p, LANE_CAP);
    w64(&p, fabric_checksum(df));

    /* lanes */
    for (int i = 0; i < PIPE_POOL; i++) {
        const DeltaLane *lane = &df->lanes[i];
        w8(&p, lane->count);
        /* 7 pad bytes — zeroed, reserved for P2 GhostRef face bitmap */
        for (int j = 0; j < 7; j++) w8(&p, 0x00);
        for (int j = 0; j < LANE_CAP; j++)
            w64(&p, (j < lane->count) ? lane->cores[j] : 0ULL);
        w64(&p, lane->checksum);
    }

    /* footer (before crc) */
    w32(&p, df->total_written);
    w32(&p, df->total_collisions);

    /* crc32 over all bytes written so far */
    size_t data_len = (size_t)(p - buf);
    uint32_t crc = crc32_xor(buf, data_len);
    w32(&p, crc);

    return (int)(p - buf);
}

/*
 * fabric_deserialize — portable binary snapshot → DeltaFabric
 *
 * Returns: bytes consumed, or negative error code
 *   -1 = buffer too small
 *   -2 = magic mismatch
 *   -3 = version mismatch
 *   -4 = lane/cap mismatch (format incompatible)
 *   -5 = crc32 integrity fail
 *   -6 = global checksum mismatch (data corruption)
 */
static int fabric_deserialize(DeltaFabric *df, const uint8_t *buf, size_t len) {
    size_t needed = fabric_snapshot_size();
    if (len < needed) return -1;

    const uint8_t *p = buf;

    /* header */
    uint32_t magic   = r32(&p);  if (magic   != SNAP_MAGIC)    return -2;
    uint32_t version = r32(&p);  if (version != SNAP_VERSION)  return -3;
    uint32_t lcount  = r32(&p);
    uint32_t lcap    = r32(&p);
    if (lcount != PIPE_POOL || lcap != LANE_CAP)               return -4;
    uint64_t stored_gcs = r64(&p);

    /* lanes */
    fabric_init(df);
    for (int i = 0; i < PIPE_POOL; i++) {
        DeltaLane *lane = &df->lanes[i];
        lane->count = r8(&p);
        /* skip 7 pad bytes */
        for (int j = 0; j < 7; j++) r8(&p);
        /* cores */
        for (int j = 0; j < LANE_CAP; j++) {
            uint64_t core = r64(&p);
            if (j < lane->count) lane->cores[j] = core;
        }
        lane->checksum = r64(&p);
        df->total_written += lane->count;
    }

    /* footer */
    df->total_written      = r32(&p);
    df->total_collisions   = r32(&p);
    uint32_t stored_crc    = r32(&p);

    /* verify crc32 over data region (everything before the crc field) */
    size_t data_len = (size_t)(p - buf) - 4;
    uint32_t computed_crc = crc32_xor(buf, data_len);
    if (computed_crc != stored_crc) return -5;

    /* verify global checksum */
    if (fabric_checksum(df) != stored_gcs) return -6;

    return (int)(p - buf);
}

/* ══════════════════════════════════════════════════════════════
 * Tests T62–T72
 * ══════════════════════════════════════════════════════════════ */
#define T(name, cond) do { \
    int _r = (cond); \
    printf("[%s] %s\n", _r ? "PASS" : "FAIL", name); \
    pass += _r; total++; \
} while(0)

/* helper: build fabric from seed */
static DeltaFabric make_fabric(uint64_t genesis) {
    GeoSeed s = seed_init(genesis);
    PipelineWire pw; wire_init(&pw, s);
    scan_wire(&pw, s, 10);
    DeltaFabric df; fabric_init(&df);
    fabric_fold_wire(&df, &pw);
    return df;
}

int main(void) {
    printf("=== M6 P1 DeltaFabric Snapshot ===\n");
    int pass = 0, total = 0;

    /* ── T62: snapshot size matches expected ── */
    size_t snap_sz = fabric_snapshot_size();
    T("T62 snapshot size = 4356B", snap_sz == 4356);

    /* ── T63: serialize returns correct byte count ── */
    DeltaFabric df_orig = make_fabric(0xDEADBEEFCAFEBABEULL);
    uint8_t buf[5000];
    int written = fabric_serialize(&df_orig, buf, sizeof(buf));
    T("T63 serialize returns 4356 bytes", written == (int)snap_sz);

    /* ── T64: magic header present ── */
    uint32_t magic_check = (uint32_t)buf[0] | ((uint32_t)buf[1]<<8)
                         | ((uint32_t)buf[2]<<16) | ((uint32_t)buf[3]<<24);
    T("T64 magic header correct", magic_check == SNAP_MAGIC);

    /* ── T65: deserialize round-trip — total_written matches ── */
    DeltaFabric df_rt; memset(&df_rt, 0, sizeof(df_rt));
    int consumed = fabric_deserialize(&df_rt, buf, (size_t)written);
    T("T65 deserialize consumes 4356 bytes",   consumed == (int)snap_sz);
    T("T66 total_written round-trip",          df_rt.total_written == df_orig.total_written);
    T("T67 total_collisions round-trip",       df_rt.total_collisions == df_orig.total_collisions);

    /* ── T68: global checksum preserved ── */
    T("T68 global checksum round-trip",
      fabric_checksum(&df_rt) == fabric_checksum(&df_orig));

    /* ── T69: per-lane core data preserved ── */
    int lanes_ok = 1;
    for (int i = 0; i < PIPE_POOL && lanes_ok; i++) {
        if (df_rt.lanes[i].count != df_orig.lanes[i].count) { lanes_ok = 0; break; }
        for (int j = 0; j < df_orig.lanes[i].count && lanes_ok; j++)
            if (df_rt.lanes[i].cores[j] != df_orig.lanes[i].cores[j]) lanes_ok = 0;
    }
    T("T69 all lane cores preserved",          lanes_ok);

    /* ── T70: per-lane checksum preserved ── */
    int cs_ok = 1;
    for (int i = 0; i < PIPE_POOL && cs_ok; i++)
        if (df_rt.lanes[i].checksum != df_orig.lanes[i].checksum) cs_ok = 0;
    T("T70 all lane checksums preserved",      cs_ok);

    /* ── T71: buffer too small → error ── */
    uint8_t small_buf[10];
    int err = fabric_serialize(&df_orig, small_buf, sizeof(small_buf));
    T("T71 small buffer returns -1",           err == -1);

    /* ── T72: tampered byte → crc fail ── */
    uint8_t tampered[5000];
    memcpy(tampered, buf, (size_t)written);
    tampered[100] ^= 0xFF;   /* flip bits in lane data region */
    DeltaFabric df_bad; memset(&df_bad, 0, sizeof(df_bad));
    int terr = fabric_deserialize(&df_bad, tampered, (size_t)written);
    T("T72 tampered data → crc error (-5 or -6)", terr == -5 || terr == -6);

    /* ── summary ── */
    printf("\n--- M6 P1 Summary ---\n");
    printf("  snapshot size:   %zu bytes\n", snap_sz);
    printf("  total_written:   %u\n", df_orig.total_written);
    printf("  global checksum: 0x%016lX\n", fabric_checksum(&df_orig));
    printf("  active lanes:    ");
    int active = 0;
    for (int i=0; i<PIPE_POOL; i++) if (df_orig.lanes[i].count > 0) active++;
    printf("%d / %d\n", active, PIPE_POOL);

    /* P2 readiness note */
    printf("\n  [P2 slots] pad[7] per lane reserved for GhostRef face bitmap\n");
    printf("  Align: lane_idx == pipe_slot %% %d → GhostRef.face_idx maps here\n",
           PIPE_POOL);

    printf("\n%d/%d PASS\n", pass, total);
    return pass == total ? 0 : 1;
}
