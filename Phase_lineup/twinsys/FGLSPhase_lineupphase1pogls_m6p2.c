/*
 * pogls_m6p2.c — M6 P2: GhostRef Serialize into DeltaFabric
 * ===========================================================
 * Extends P1 snapshot format:
 *   pad[7] per lane now carries GhostRef face_idx bitmap
 *
 * Encoding (pad[7] layout, frozen):
 *   pad[0] = face_bitmap  : bit i = 1 if any blueprint in this lane has face_idx==i (bits 0-5)
 *   pad[1] = bp_count_in_lane : how many blueprints mapped to this lane (0-8 cap = LANE_CAP)
 *   pad[2..6] = reserved / zeroed (future)
 *
 * Mapping rule (P1 lock):
 *   blueprint[i] → lane_idx = i % PIPE_POOL (54)
 *   core         = blueprint[i].master_core
 *   face_idx     = blueprint[i].face_idx
 *
 * ghost_fabric_write() : WatcherCtx → DeltaFabric
 * ghost_fabric_read()  : DeltaFabric lanes → face bitmap + bp_count per lane
 *
 * Format compatibility: pad[7] was zeroed in P1 → P2 readers see
 *   pad[0]==0 / pad[1]==0 on P1 snapshots (graceful degradation)
 *
 * Tests T73–T83 (11 tests)
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* ══════════════════════════════════════════════════════════════
 * Carry-forward from pogls_m6.c (P1 primitives + structs)
 * ══════════════════════════════════════════════════════════════ */

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

#define PIPE_SLOTS   15
#define PIPE_POOL    54
#define LANE_CAP      8
#define SNAP_MAGIC   0x504C4601UL
#define SNAP_VERSION 1

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
    /* P2: pad[7] embedded in serialized form only — not stored in struct */
    /* pad[0] = face_bitmap, pad[1] = bp_count_in_lane, pad[2-6] = 0 */
} DeltaLane;
typedef struct {
    DeltaLane lanes[PIPE_POOL];
    uint32_t  total_written;
    uint32_t  total_collisions;
} DeltaFabric;

/* P2: per-lane pad data (decoded from snapshot) */
typedef struct {
    uint8_t face_bitmap;       /* bits 0-5: which face_idx values present */
    uint8_t bp_count_in_lane;  /* how many blueprints mapped to this lane  */
    uint8_t _reserved[5];      /* pad[2-6]: reserved, must be 0            */
} LanePad;

/* GhostRef (from geo_ghost_watcher.h — self-contained here) */
typedef struct {
    uint64_t master_core;
    uint8_t  face_idx;
} GhostRef;

#define GHOST_MAX_BLUEPRINTS 144u

typedef struct {
    GhostRef blueprints[GHOST_MAX_BLUEPRINTS];
    uint32_t bp_count;
} WatcherCtx;  /* minimal — just the fields P2 needs */

/* ── M5 carry-forward helpers ── */
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

/* ── P1 serialize primitives ── */
static inline void w8(uint8_t **p, uint8_t v)   { **p = v; (*p)++; }
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
static inline size_t fabric_snapshot_size(void) {
    return 24 + (size_t)PIPE_POOL * (1 + 7 + (size_t)LANE_CAP * 8 + 8) + 12;
}

/* ══════════════════════════════════════════════════════════════
 * P2 NEW: ghost_fabric_write
 * Write WatcherCtx.blueprints into DeltaFabric
 *   blueprint[i] → lane_idx = i % PIPE_POOL
 *   core = master_core (keyed into lane cores[])
 *   face_idx tracked in per-lane pad data (encoded at serialize time)
 * Returns: number of blueprints written (collisions excluded)
 * ══════════════════════════════════════════════════════════════ */
static uint32_t ghost_fabric_write(DeltaFabric *df, const WatcherCtx *w) {
    uint32_t written = 0;
    for (uint32_t i = 0; i < w->bp_count; i++) {
        uint8_t lane_idx = (uint8_t)(i % PIPE_POOL);
        if (fabric_write(df, lane_idx, w->blueprints[i].master_core) != 0xFF)
            written++;
    }
    return written;
}

/* ══════════════════════════════════════════════════════════════
 * P2 NEW: build_lane_pads
 * Pre-compute LanePad[PIPE_POOL] from WatcherCtx blueprints
 * Called before serialize to populate pad[0..1] per lane
 * ══════════════════════════════════════════════════════════════ */
static void build_lane_pads(LanePad pads[PIPE_POOL], const WatcherCtx *w) {
    memset(pads, 0, sizeof(LanePad) * PIPE_POOL);
    for (uint32_t i = 0; i < w->bp_count; i++) {
        uint8_t lane_idx = (uint8_t)(i % PIPE_POOL);
        uint8_t face     = w->blueprints[i].face_idx & 0x07u;  /* clamp 0-7 */
        pads[lane_idx].face_bitmap      |= (uint8_t)(1u << face);
        if (pads[lane_idx].bp_count_in_lane < 0xFF)
            pads[lane_idx].bp_count_in_lane++;
    }
}

/* ══════════════════════════════════════════════════════════════
 * P2 serialize — identical to P1 but writes pad[0..1] from pads[]
 * Signature extended: takes optional LanePad array
 *   pads == NULL → P1 behaviour (all pad bytes zeroed)
 * ══════════════════════════════════════════════════════════════ */
static int fabric_serialize_p2(const DeltaFabric *df,
                                const LanePad     *pads,   /* NULL = P1 mode */
                                uint8_t           *buf,
                                size_t             cap)
{
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
        /* pad[0]: face_bitmap  | pad[1]: bp_count_in_lane | pad[2-6]: 0 */
        if (pads) {
            w8(&p, pads[i].face_bitmap);
            w8(&p, pads[i].bp_count_in_lane);
            for (int j = 2; j < 7; j++) w8(&p, 0x00);
        } else {
            for (int j = 0; j < 7; j++) w8(&p, 0x00);  /* P1 compat */
        }
        for (int j = 0; j < LANE_CAP; j++)
            w64(&p, (j < lane->count) ? lane->cores[j] : 0ULL);
        w64(&p, lane->checksum);
    }

    /* footer */
    w32(&p, df->total_written);
    w32(&p, df->total_collisions);

    size_t data_len = (size_t)(p - buf);
    uint32_t crc = crc32_xor(buf, data_len);
    w32(&p, crc);

    return (int)(p - buf);
}

/* ══════════════════════════════════════════════════════════════
 * P2 deserialize — reads pad[0..1] into LanePad array
 * pads_out: caller buffer [PIPE_POOL], may be NULL (skip pad decode)
 * Error codes same as P1 (-1..-6)
 * ══════════════════════════════════════════════════════════════ */
static int fabric_deserialize_p2(DeltaFabric *df,
                                  LanePad      *pads_out,   /* may be NULL */
                                  const uint8_t *buf,
                                  size_t         len)
{
    size_t needed = fabric_snapshot_size();
    if (len < needed) return -1;

    const uint8_t *p = buf;

    uint32_t magic   = r32(&p);  if (magic   != SNAP_MAGIC)   return -2;
    uint32_t version = r32(&p);  if (version != SNAP_VERSION) return -3;
    uint32_t lcount  = r32(&p);
    uint32_t lcap    = r32(&p);
    if (lcount != PIPE_POOL || lcap != LANE_CAP)              return -4;
    uint64_t stored_gcs = r64(&p);

    fabric_init(df);
    for (int i = 0; i < PIPE_POOL; i++) {
        DeltaLane *lane = &df->lanes[i];
        lane->count = r8(&p);

        /* read pad[7] */
        uint8_t pad0 = r8(&p);   /* face_bitmap        */
        uint8_t pad1 = r8(&p);   /* bp_count_in_lane   */
        for (int j = 2; j < 7; j++) r8(&p);   /* skip reserved */

        if (pads_out) {
            pads_out[i].face_bitmap      = pad0;
            pads_out[i].bp_count_in_lane = pad1;
            memset(pads_out[i]._reserved, 0, 5);
        }

        for (int j = 0; j < LANE_CAP; j++) {
            uint64_t core = r64(&p);
            if (j < lane->count) lane->cores[j] = core;
        }
        lane->checksum = r64(&p);
        df->total_written += lane->count;
    }

    df->total_written    = r32(&p);
    df->total_collisions = r32(&p);
    uint32_t stored_crc  = r32(&p);

    size_t data_len = (size_t)(p - buf) - 4;
    uint32_t computed_crc = crc32_xor(buf, data_len);
    if (computed_crc != stored_crc)           return -5;
    if (fabric_checksum(df) != stored_gcs)    return -6;

    return (int)(p - buf);
}

/* ══════════════════════════════════════════════════════════════
 * Test helpers
 * ══════════════════════════════════════════════════════════════ */

/* Build a WatcherCtx with n synthetic blueprints */
static void make_watcher(WatcherCtx *w, uint32_t n, uint64_t seed) {
    memset(w, 0, sizeof(WatcherCtx));
    if (n > GHOST_MAX_BLUEPRINTS) n = GHOST_MAX_BLUEPRINTS;
    for (uint32_t i = 0; i < n; i++) {
        w->blueprints[i].master_core = mix64(seed ^ (uint64_t)i);
        w->blueprints[i].face_idx    = (uint8_t)(i % 6);
    }
    w->bp_count = n;
}

/* Build DeltaFabric from PipelineWire (M5 path, for comparison) */
static DeltaFabric make_fabric_wire(uint64_t genesis) {
    GeoSeed s = seed_init(genesis);
    PipelineWire pw; wire_init(&pw, s);
    scan_wire(&pw, s, 10);
    DeltaFabric df; fabric_init(&df);
    fabric_fold_wire(&df, &pw);
    return df;
}

/* ══════════════════════════════════════════════════════════════
 * Tests T73–T83
 * ══════════════════════════════════════════════════════════════ */
#define T(name, cond) do { \
    int _r = (cond); \
    printf("[%s] %s\n", _r ? "PASS" : "FAIL", name); \
    pass += _r; total++; \
} while(0)

int main(void) {
    printf("=== M6 P2 GhostRef Serialize ===\n");
    int pass = 0, total = 0;

    /* ── T73: ghost_fabric_write maps blueprint[i] → lane i%54 ── */
    WatcherCtx w54; make_watcher(&w54, 54, 0xABCDEF1234567890ULL);
    DeltaFabric df54; fabric_init(&df54);
    uint32_t written54 = ghost_fabric_write(&df54, &w54);
    /* 54 blueprints → 54 lanes, each gets exactly 1 core */
    int lanes_one = 1;
    for (int i = 0; i < PIPE_POOL && lanes_one; i++)
        if (df54.lanes[i].count != 1) lanes_one = 0;
    T("T73 54 blueprints → 54 lanes, each count=1", written54 == 54 && lanes_one);

    /* ── T74: blueprint[0] core == lane[0].cores[0] ── */
    T("T74 blueprint[0].master_core == lane[0].cores[0]",
      df54.lanes[0].cores[0] == w54.blueprints[0].master_core);

    /* ── T75: blueprint[53] → lane[53] ── */
    T("T75 blueprint[53] → lane[53]",
      df54.lanes[53].cores[0] == w54.blueprints[53].master_core);

    /* ── T76: blueprint[54] wraps → lane[0] (overflow into count=2) ── */
    WatcherCtx w108; make_watcher(&w108, 108, 0x1122334455667788ULL);
    DeltaFabric df108; fabric_init(&df108);
    uint32_t written108 = ghost_fabric_write(&df108, &w108);
    /* 108 = 2 × 54 → each lane gets count=2 */
    int lanes_two = 1;
    for (int i = 0; i < PIPE_POOL && lanes_two; i++)
        if (df108.lanes[i].count != 2) lanes_two = 0;
    T("T76 108 blueprints wraps → each lane count=2", written108 == 108 && lanes_two);

    /* ── T77: build_lane_pads — face_bitmap correct ── */
    /* blueprint[i].face_idx = i%6, so lane[0] gets blueprint[0](face=0),
       blueprint[54](face=0) → bitmap = 0b000001 = 0x01 */
    LanePad pads108[PIPE_POOL];
    build_lane_pads(pads108, &w108);
    /* lane[0]: blueprints [0,54] → face 0%6=0, 54%6=0 → bitmap bit0 set */
    T("T77 lane[0] face_bitmap = 0x01 (face 0 only)",
      pads108[0].face_bitmap == 0x01);

    /* ── T78: build_lane_pads — bp_count_in_lane correct ── */
    T("T78 lane[0] bp_count_in_lane = 2", pads108[0].bp_count_in_lane == 2);

    /* ── T79: lane[1] face_bitmap = 0x02 (face 1 only: bp[1]→f1, bp[55]→f1) ── */
    T("T79 lane[1] face_bitmap = 0x02 (face 1)",
      pads108[1].face_bitmap == 0x02);

    /* ── T80: serialize P2 round-trip — pad bytes survive ── */
    DeltaFabric df_orig = make_fabric_wire(0xDEADBEEFCAFEBABEULL);
    WatcherCtx w6; make_watcher(&w6, 6, 0xFEEDC0FFEE000000ULL);
    ghost_fabric_write(&df_orig, &w6);  /* wire 6 blueprints into fabric */

    LanePad pads_orig[PIPE_POOL];
    build_lane_pads(pads_orig, &w6);

    uint8_t buf[5000];
    int sz = fabric_serialize_p2(&df_orig, pads_orig, buf, sizeof(buf));
    T("T80 serialize_p2 returns 4356B", sz == (int)fabric_snapshot_size());

    /* ── T81: deserialize P2 — pad[0]/[1] round-trip ── */
    DeltaFabric df_rt;
    LanePad pads_rt[PIPE_POOL];
    int consumed = fabric_deserialize_p2(&df_rt, pads_rt, buf, (size_t)sz);
    T("T81 deserialize_p2 consumes 4356B", consumed == (int)fabric_snapshot_size());

    /* lane[0] got blueprint[0] (face=0) → bitmap=0x01, count=1 */
    T("T82 pad round-trip: lane[0] face_bitmap preserved",
      pads_rt[0].face_bitmap == pads_orig[0].face_bitmap);
    T("T83 pad round-trip: lane[0] bp_count_in_lane preserved",
      pads_rt[0].bp_count_in_lane == pads_orig[0].bp_count_in_lane);

    /* summary */
    printf("\n--- M6 P2 Summary ---\n");
    printf("  blueprints (54-set): written=%u\n", written54);
    printf("  blueprints (108-set): written=%u\n", written108);
    printf("  pad round-trip: lane[0] bitmap=0x%02X count=%u\n",
           pads_rt[0].face_bitmap, pads_rt[0].bp_count_in_lane);
    printf("  snapshot size: %zu B (format unchanged)\n", fabric_snapshot_size());
    printf("  [P3 ready] face_bitmap queryable per lane after deserialize\n");

    printf("\n%d/%d PASS\n", pass, total);
    return pass == total ? 0 : 1;
}
