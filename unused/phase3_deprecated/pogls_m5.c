#include <stdio.h>
#include <stdint.h>
#include <string.h>

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
#define FIBO_FLUSH   720
#define TE_CYCLE     144
#define GHOST_POOL   54

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
typedef struct { uint64_t master_core; uint8_t face_idx; } GhostRef;

static inline GeoSeed seed_init(uint64_t genesis) {
    GeoSeed s = { genesis, mix64(genesis) };
    for (int t = 0; t < 64; t++) {
        for (uint8_t a = 0; a < 6; a++)
            for (uint8_t b = a+1; b < 6; b++)
                if (!is_complement(a,b) &&
                    fibo_apex(slope(s.gen2,a), slope(s.gen2,b)))
                    return s;
        s.gen2 = mix64(s.gen2);
    }
    return s;
}
static inline void wire_init(PipelineWire *pw, GeoSeed s) {
    memset(pw, 0, sizeof(PipelineWire));
    pw->seed_core       = s.gen2;
    pw->slots[0].core   = s.gen2;
    pw->slots[0].face   = 0;
    pw->slots[0].depth  = 0;
    pw->slots[0].occupied = 1;
}
static inline uint8_t wire_node(PipelineWire *pw,
                                 uint64_t core, uint8_t face, uint8_t depth) {
    uint8_t idx = (uint8_t)(slope(core, face) % PIPE_SLOTS);
    PipeSlot *s = &pw->slots[idx];
    if (!s->occupied || s->depth > depth) {
        s->core = core; s->face = face;
        s->depth = depth; s->occupied = 1;
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

/* M5.1 DeltaLane */
#define LANE_CAP  8
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

static inline void fabric_init(DeltaFabric *df) {
    memset(df, 0, sizeof(DeltaFabric));
}
static inline uint8_t fabric_write(DeltaFabric *df,
                                    uint8_t pipe_slot_idx, uint64_t core) {
    uint8_t lane_idx = pipe_slot_idx % PIPE_POOL;
    DeltaLane *lane  = &df->lanes[lane_idx];
    if (lane->count >= LANE_CAP) { df->total_collisions++; return 0xFF; }
    lane->cores[lane->count++] = core;
    lane->checksum ^= core;
    df->total_written++;
    return lane_idx;
}
static inline uint32_t fabric_fold_wire(DeltaFabric *df, const PipelineWire *pw) {
    uint32_t written = 0;
    for (uint8_t i = 0; i < PIPE_SLOTS; i++) {
        if (!pw->slots[i].occupied) continue;
        uint8_t r = fabric_write(df, i, pw->slots[i].core);
        if (r != 0xFF) written++;
    }
    return written;
}
static inline uint64_t fabric_checksum(const DeltaFabric *df) {
    uint64_t acc = 0;
    for (int i = 0; i < PIPE_POOL; i++) acc ^= df->lanes[i].checksum;
    return acc;
}

/* M5.2 scan/replay */
/* ScanChain: record the actually-wired core sequence for replay */
#define SCAN_CHAIN_MAX PIPE_SLOTS
typedef struct {
    uint64_t cores[SCAN_CHAIN_MAX];
    uint8_t  faces[SCAN_CHAIN_MAX];
    uint8_t  slots[SCAN_CHAIN_MAX];
    uint32_t count;
} ScanChain;

static inline uint32_t scan_wire(PipelineWire *pw, GeoSeed s, uint32_t n) {
    uint32_t wired = 0;
    uint64_t cur   = s.gen2;
    for (uint32_t i = 0; i < n && i < PIPE_SLOTS; i++) {
        uint8_t face = (uint8_t)(i % 6);
        uint64_t pat  = slope(cur, face) ^ slope(cur, (face+1)%6);
        uint64_t next = derive_child(cur, pat, (uint8_t)(i+1));
        uint8_t r = wire_node(pw, next, face, (uint8_t)(i % 4));
        if (r != 0xFF) { wired++; cur = next; }
    }
    return wired;
}

/* scan_wire_chain: scan + record wired path for deterministic replay */
static inline uint32_t scan_wire_chain(PipelineWire *pw, GeoSeed s,
                                        uint32_t n, ScanChain *sc) {
    uint32_t wired = 0;
    uint64_t cur   = s.gen2;
    sc->count = 0;
    for (uint32_t i = 0; i < n && i < PIPE_SLOTS; i++) {
        uint8_t face = (uint8_t)(i % 6);
        uint64_t pat  = slope(cur, face) ^ slope(cur, (face+1)%6);
        uint64_t next = derive_child(cur, pat, (uint8_t)(i+1));
        uint8_t r = wire_node(pw, next, face, (uint8_t)(i % 4));
        if (r != 0xFF) {
            sc->cores[sc->count] = next;
            sc->faces[sc->count] = face;
            sc->slots[sc->count] = r;
            sc->count++;
            wired++; cur = next;
        }
    }
    return wired;
}
/* replay_chain: verify wired chain against pipeline slots
 * Uses ScanChain (actual wired path) — not re-derived path
 * Invariant: every wired core must still occupy its slot         */
static inline int replay_chain(const PipelineWire *pw, const ScanChain *sc) {
    for (uint32_t i = 0; i < sc->count; i++) {
        const PipeSlot *ps = &pw->slots[sc->slots[i]];
        if (!ps->occupied)           return 0;
        if (ps->core != sc->cores[i]) return 0;
    }
    return 1;
}

static inline int replay_wire(const PipelineWire *pw, GeoSeed s, uint32_t n) {
    (void)pw; (void)s; (void)n;
    return 1; /* legacy stub — use scan_wire_chain + replay_chain */
}

/* M5.3 EvictStats */
typedef struct { uint32_t rejected; uint32_t evicted; uint32_t kept; } EvictStats;
static inline uint8_t wire_node_tracked(PipelineWire *pw,
                                         uint64_t core, uint8_t face,
                                         uint8_t depth, EvictStats *es) {
    uint8_t idx = (uint8_t)(slope(core, face) % PIPE_SLOTS);
    PipeSlot *s = &pw->slots[idx];
    if (!s->occupied) {
        s->core=core; s->face=face; s->depth=depth; s->occupied=1;
        pw->tick++; return idx;
    }
    if (s->depth > depth)  { es->evicted++; s->core=core; s->face=face; s->depth=depth; pw->tick++; return idx; }
    if (s->depth == depth) { es->kept++;    return 0xFF; }
    es->rejected++;
    return 0xFF;
}

/* Tests T48-T61 */
#define T(name, cond) do { \
    int _r = (cond); \
    printf("[%s] %s\n", _r ? "PASS" : "FAIL", name); \
    pass += _r; total++; \
} while(0)

int main(void) {
    printf("=== M5 Delta Fabric ===\n");
    int pass = 0, total = 0;

    GeoSeed s0 = seed_init(0xDEADBEEFCAFEBABEULL);

    /* M5.1 */
    DeltaFabric df; fabric_init(&df);
    uint8_t r0 = fabric_write(&df, 0, s0.gen2);
    T("T48 write lane returns correct idx",  r0 == 0);
    T("T49 lane 0 count increments",         df.lanes[0].count == 1);
    T("T50 lane 0 checksum == core",         df.lanes[0].checksum == s0.gen2);

    uint64_t core2 = mix64(s0.gen2 ^ 0xABCDULL);
    uint8_t r1 = fabric_write(&df, 15, core2);
    T("T51 slot 15 → lane 15",               r1 == 15);

    DeltaFabric df2; fabric_init(&df2);
    fabric_write(&df2, 1, 0xAAAAAAAAAAAAAAAAULL);
    fabric_write(&df2, 1, 0xAAAAAAAAAAAAAAAAULL);
    T("T52 XOR fold self-cancels",           df2.lanes[1].checksum == 0);

    DeltaFabric df3; fabric_init(&df3);
    for (int i = 0; i < LANE_CAP; i++) fabric_write(&df3, 2, (uint64_t)i + 1);
    uint8_t overflow = fabric_write(&df3, 2, 0xFFFFULL);
    T("T53 lane cap rejects overflow",       overflow == 0xFF);
    T("T54 collision counter increments",    df3.total_collisions == 1);

    PipelineWire pw; wire_init(&pw, s0);
    scan_wire(&pw, s0, 8);
    DeltaFabric df4; fabric_init(&df4);
    uint32_t written = fabric_fold_wire(&df4, &pw);
    T("T55 fold_wire writes occupied slots", written == wire_occupancy(&pw));
    T("T56 fabric checksum non-zero",        fabric_checksum(&df4) != 0);

    PipelineWire pw2; wire_init(&pw2, s0);
    scan_wire(&pw2, s0, 8);
    DeltaFabric df5; fabric_init(&df5);
    fabric_fold_wire(&df5, &pw2);
    T("T57 checksum deterministic",          fabric_checksum(&df4) == fabric_checksum(&df5));

    /* M5.2 */
    PipelineWire pw3; wire_init(&pw3, s0);
    ScanChain sc; memset(&sc, 0, sizeof(sc));
    uint32_t wired = scan_wire_chain(&pw3, s0, 6, &sc);
    int rt = replay_chain(&pw3, &sc);
    T("T58 scan wires nodes",                wired > 0);
    T("T59 replay round-trip passes",        rt == 1);

    GeoSeed s1 = seed_init(0x1234567890ABCDEFULL);
    PipelineWire pw4; wire_init(&pw4, s1);
    scan_wire(&pw4, s1, 6);
    DeltaFabric df6; fabric_init(&df6);
    fabric_fold_wire(&df6, &pw4);
    T("T60 diff seed → diff checksum",       fabric_checksum(&df4) != fabric_checksum(&df6));

    /* M5.3 */
    PipelineWire pw5; wire_init(&pw5, s0);
    EvictStats es = {0};
    uint64_t cn = mix64(s0.gen2 ^ 0x1ULL);
    wire_node_tracked(&pw5, cn, 1, 0, &es);
    wire_node_tracked(&pw5, cn ^ 0x1ULL, 1, 0, &es);
    wire_node_tracked(&pw5, mix64(cn), 1, 3, &es);
    T("T61 evict stats: kept+rejected > 0",  es.kept + es.rejected > 0);

    printf("\n--- M5 Fabric summary ---\n");
    printf("  seed gen2:      0x%016lX\n", s0.gen2);
    printf("  pipeline occ:   %d/%d slots\n", wire_occupancy(&pw), PIPE_SLOTS);
    printf("  fabric written: %u\n", written);
    printf("  checksum(df4):  0x%016lX\n", fabric_checksum(&df4));
    printf("  evict stats:    evicted=%u kept=%u rejected=%u\n",
           es.evicted, es.kept, es.rejected);
    printf("\n%d/%d PASS\n", pass, total);
    return pass == total ? 0 : 1;
}
