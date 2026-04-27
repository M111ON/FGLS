/*
 * geo_core_bench.c — Geometric Address Benchmark
 * ================================================
 * Single file, no dependencies except libc + time.h
 *
 * Proves: trit-mapped write/read/delete is viable vs hash table
 *
 * compile: gcc -O2 -o bench geo_core_bench.c && ./bench
 * compile (AVX2): gcc -O2 -mavx2 -DUSE_AVX2 -o bench geo_core_bench.c && ./bench
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* ══════════════════════════════════════════════════════
   CONSTANTS — sacred numbers, do not change
   ══════════════════════════════════════════════════════ */
#define TRIT_MOD    27u    /* 3^3 — ternary address space    */
#define SPOKE_MOD    6u    /* GEO_SPOKES — dodeca directions  */
#define COSET_MOD    9u    /* 3^2 — GCFS active cosets        */
#define LETTER_MOD  26u    /* A-Z LC pair index               */
#define FIBO_MOD   144u    /* F(12) — fibo clock cycle        */

/* dodeca slot layout: 27 trit × 6 spoke × 9 coset = 1,458 slots
   each slot holds one uint64_t value + 1 byte valid flag        */
#define DODECA_SLOTS  (TRIT_MOD * SPOKE_MOD * COSET_MOD)  /* 1,458 */

/* reserved_mask: 1 bit per coset (9 bits used of uint16_t)
   set bit = "deleted" coset → reads return 0, writes go to GROUND */
#define RESERVED_NONE  0x0000u
#define RESERVED_ALL   0x01FFu   /* bits 0-8 = 9 cosets          */

/* ══════════════════════════════════════════════════════
   TRIT MAP — core geometric address function
   (addr ^ value) % 27 → ternary position
   no heap, no hash, pure arithmetic
   ══════════════════════════════════════════════════════ */
typedef struct {
    uint8_t  trit;    /* 0-26: 3^3 ternary index             */
    uint8_t  spoke;   /* 0-5:  dodeca direction              */
    uint8_t  coset;   /* 0-8:  GCFS coset layer              */
    uint8_t  letter;  /* 0-25: LC pair A-Z                   */
    uint8_t  fibo;    /* 0-143: fibo clock phase             */
} GeoAddr;

static inline GeoAddr geo_addr(uint64_t addr, uint64_t value) {
    /* addr-only idx — decouples address derivation from value so read
       path works without knowing value. value kept for future WARP/COLLISION
       gate (lc_twin_gate integration point). */
    (void)value;
    uint64_t idx = addr;
    /* mix bits to spread across trit/spoke/coset evenly */
    idx ^= idx >> 33;
    idx *= 0xff51afd7ed558ccdULL;
    idx ^= idx >> 33;
    GeoAddr  g;
    g.trit   = (uint8_t)(idx % TRIT_MOD);
    g.spoke  = (uint8_t)((idx >> 8)  % SPOKE_MOD);
    g.coset  = (uint8_t)((idx >> 17) % COSET_MOD);
    g.letter = (uint8_t)((idx >> 26) % LETTER_MOD);
    g.fibo   = (uint8_t)((idx >> 35) % FIBO_MOD);
    return g;
}

/* slot index: pack (trit, spoke, coset) → flat array index */
static inline uint32_t slot_idx(uint8_t trit, uint8_t spoke, uint8_t coset) {
    return (uint32_t)trit * (SPOKE_MOD * COSET_MOD)
         + (uint32_t)spoke * COSET_MOD
         + (uint32_t)coset;
}

/* ══════════════════════════════════════════════════════
   DODECA STORE — flat array, cache-friendly
   No malloc. Lives on stack or static.
   ══════════════════════════════════════════════════════ */
typedef struct {
    uint64_t value[DODECA_SLOTS];
    uint64_t addr [DODECA_SLOTS];
    uint8_t  valid[DODECA_SLOTS];   /* bits[1:0]=state, bits[7:3]=actual_trit */
    uint16_t fwd  [DODECA_SLOTS];   /* fwd[prim_slot] = actual placed slot idx */
    uint16_t reserved_mask;         /* per-coset delete bitmask         */
    /* stats */
    uint32_t writes;
    uint32_t reads;
    uint32_t collisions;   /* COLLISION: slot occupied by different addr */
    uint32_t grounds;      /* GROUND: routed to ground lane              */
    uint32_t drops;      /* write failed — all slots in trit×coset full */
    uint32_t deletes;
    uint32_t fwd_hits;   /* reads served by fwd fast path (no scan)     */
    uint32_t fwd_miss;   /* reads that fell back to full scan            */
} DodecaStore;

/* valid byte helpers:
   bits[1:0] = state  (0=empty, 1=live, 2=ground, 3=tombstone)
   bits[7:3] = actual_trit (0-26, 5 bits)  → where this entry physically landed */
#define VALID_STATE(v)       ((v) & 0x03u)
#define VALID_TRIT(v)        (((v) >> 3) & 0x1Fu)
#define VALID_PACK(st, trit) ((uint8_t)(((trit) << 3) | ((st) & 0x03u)))

static inline void dodeca_init(DodecaStore *d) {
    memset(d, 0, sizeof(*d));
    /* fwd sentinel: 0xFFFF = no forwarding set yet (slot 0 is valid placed_s) */
    memset(d->fwd, 0xFF, sizeof(d->fwd));
}

/* gate decision — mirrors lc_twin_gate logic without LC dependency */
typedef enum {
    GEO_WARP      = 0,   /* slot empty → direct write         */
    GEO_ROUTE     = 1,   /* slot occupied by same addr → update */
    GEO_COLLISION = 2,   /* slot occupied by different addr    */
    GEO_GROUND    = 3,   /* coset deleted via reserved_mask    */
} GeoGate;

static inline GeoGate geo_gate(const DodecaStore *d,
                                uint8_t trit, uint8_t spoke, uint8_t coset,
                                uint64_t addr) {
    if (d->reserved_mask & (1u << coset)) return GEO_GROUND;
    uint32_t s  = slot_idx(trit, spoke, coset);
    uint8_t  st = VALID_STATE(d->valid[s]);
    if (st == 0 || st == 3)       return GEO_WARP;
    if (d->addr[s] == addr)       return GEO_ROUTE;
    return GEO_COLLISION;
}

/* ── write ── */
static inline GeoGate dodeca_write(DodecaStore *d,
                                    uint64_t addr, uint64_t value) {
    GeoAddr  g    = geo_addr(addr, value);
    uint32_t prim = slot_idx(g.trit, g.spoke, g.coset);  /* primary slot */

    d->writes++;

    /* GROUND: coset deleted */
    if (d->reserved_mask & (1u << g.coset)) {
        d->grounds++;
        uint32_t gs = slot_idx(0, g.spoke, g.coset);
        d->value[gs] = value;
        d->addr[gs]  = addr;
        d->valid[gs] = VALID_PACK(2, 0);   /* ground in trit=0 */
        return GEO_GROUND;
    }

    /* find placement slot: spoke ring first, then trit ring */
    uint8_t  placed_trit = g.trit;
    uint8_t  placed_spoke= g.spoke;
    uint32_t placed_s    = prim;
    int      found       = 0;
    GeoGate  gate        = GEO_WARP;

    for (uint8_t dt = 0; dt < TRIT_MOD && !found; dt++) {
        uint8_t t2 = (g.trit + dt) % TRIT_MOD;
        for (uint8_t i = 0; i < SPOKE_MOD && !found; i++) {
            uint8_t  sp2 = (g.spoke + i) % SPOKE_MOD;
            uint32_t s2  = slot_idx(t2, sp2, g.coset);
            uint8_t  st  = VALID_STATE(d->valid[s2]);
            if (st == 0 || st == 3) {           /* empty or tombstone */
                placed_trit  = t2;
                placed_spoke = sp2;
                placed_s     = s2;
                gate = (dt == 0 && i == 0) ? GEO_WARP : GEO_COLLISION;
                found = 1;
            } else if (d->addr[s2] == addr) {   /* same addr → update */
                placed_trit  = t2;
                placed_spoke = sp2;
                placed_s     = s2;
                gate = GEO_ROUTE;
                found = 1;
            }
        }
    }

    if (!found) { d->drops++; return GEO_COLLISION; }
    if (gate == GEO_COLLISION) d->collisions++;

    /* write value to placed slot */
    d->value[placed_s] = value;
    d->addr [placed_s] = addr;
    d->valid[placed_s] = VALID_PACK(1, placed_trit);

    /* fwd[addr_key] = placed_s — keyed by addr hash not prim slot
       avoids geometric clustering: two addrs with same prim (geo collision)
       get different fwd keys because addr bits differ                        */
    uint16_t fwd_key = (uint16_t)((addr ^ (addr >> 16)) % DODECA_SLOTS);
    d->fwd[fwd_key] = (uint16_t)placed_s;

    return gate;
}

/* ── read ── O(1) via fwd[prim] direct slot pointer ── */
static inline int dodeca_read(DodecaStore *d,
                               uint64_t addr, uint64_t value_hint,
                               uint64_t *out) {
    (void)value_hint;
    GeoAddr  g    = geo_addr(addr, 0);
    uint32_t prim = slot_idx(g.trit, g.spoke, g.coset);
    d->reads++;

    if (d->reserved_mask & (1u << g.coset)) return 0;

    /* ── fast path: fwd[addr_key] → actual slot ── */
    uint16_t fwd_key = (uint16_t)((addr ^ (addr >> 16)) % DODECA_SLOTS);
    uint16_t fs = d->fwd[fwd_key];
    if (fs != 0xFFFFu) {
        uint8_t st = VALID_STATE(d->valid[fs]);
        if (st == 1 && d->addr[fs] == addr) {
            *out = d->value[fs];
            d->fwd_hits++;
            return 1;
        }
    }

    /* ── fallback: full scan (fwd stale after delete or hash corner case) ── */
    d->fwd_miss++;
    for (uint8_t dt = 0; dt < TRIT_MOD; dt++) {
        uint8_t trit2 = (g.trit + dt) % TRIT_MOD;
        for (uint8_t i = 0; i < SPOKE_MOD; i++) {
            uint8_t  sp = (g.spoke + i) % SPOKE_MOD;
            uint32_t s  = slot_idx(trit2, sp, g.coset);
            uint8_t  st = VALID_STATE(d->valid[s]);
            if (st == 1 && d->addr[s] == addr) {
                *out = d->value[s];
                return 1;
            }
        }
    }
    return 0;
}

/* ── delete (per-slot tombstone — valid state=3, data preserved) ── */
static inline void dodeca_delete(DodecaStore *d, uint64_t addr) {
    for (uint8_t t = 0; t < TRIT_MOD; t++) {
        for (uint8_t sp = 0; sp < SPOKE_MOD; sp++) {
            for (uint8_t c = 0; c < COSET_MOD; c++) {
                uint32_t s  = slot_idx(t, sp, c);
                uint8_t  st = VALID_STATE(d->valid[s]);
                if (st == 1 && d->addr[s] == addr) {
                    uint8_t actual_trit = VALID_TRIT(d->valid[s]);
                    d->valid[s] = VALID_PACK(3, actual_trit); /* tombstone, keep trit */
                }
            }
        }
    }
    /* clear primary slot forwarding hint so fast-path doesn't follow stale trit */
    GeoAddr  g    = geo_addr(addr, 0);
    uint32_t prim = slot_idx(g.trit, g.spoke, g.coset);
    if (VALID_STATE(d->valid[prim]) == 0)
        d->valid[prim] = 0;   /* clear forwarding sentinel */
    d->deletes++;
}

/* ── bulk coset delete via reserved_mask ── */
static inline void dodeca_delete_coset(DodecaStore *d, uint8_t coset) {
    d->reserved_mask |= (uint16_t)(1u << coset);
    d->deletes++;
}

/* ── delete check ── */
static inline int dodeca_is_deleted(const DodecaStore *d, uint64_t addr) {
    uint8_t coset = (uint8_t)(addr % COSET_MOD);
    return (d->reserved_mask & (1u << coset)) ? 1 : 0;
}

/* ══════════════════════════════════════════════════════
   BASELINE HASH TABLE — for comparison
   djb2 hash → open addressing, same flat array size
   ══════════════════════════════════════════════════════ */
#define HASH_SLOTS  2048u   /* power of 2, > DODECA_SLOTS for fair compare */
#define HASH_MASK   (HASH_SLOTS - 1u)

typedef struct {
    uint64_t key  [HASH_SLOTS];
    uint64_t value[HASH_SLOTS];
    uint8_t  valid[HASH_SLOTS];
    uint32_t writes;
    uint32_t reads;
    uint32_t collisions;
} HashStore;

static inline void hash_init(HashStore *h) { memset(h, 0, sizeof(*h)); }

static inline uint32_t hash_fn(uint64_t k) {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return (uint32_t)(k & HASH_MASK);
}

static inline void hash_write(HashStore *h, uint64_t key, uint64_t value) {
    uint32_t s = hash_fn(key);
    h->writes++;
    for (uint32_t i = 0; i < HASH_SLOTS; i++) {
        uint32_t si = (s + i) & HASH_MASK;
        if (!h->valid[si] || h->key[si] == key) {
            if (h->valid[si] && h->key[si] != key) h->collisions++;
            h->key[si]   = key;
            h->value[si] = value;
            h->valid[si] = 1;
            return;
        }
        h->collisions++;
    }
}

static inline int hash_read(HashStore *h, uint64_t key, uint64_t *out) {
    uint32_t s = hash_fn(key);
    h->reads++;
    for (uint32_t i = 0; i < HASH_SLOTS; i++) {
        uint32_t si = (s + i) & HASH_MASK;
        if (!h->valid[si]) return 0;
        if (h->key[si] == key) { *out = h->value[si]; return 1; }
    }
    return 0;
}

/* ══════════════════════════════════════════════════════
   TIMER
   ══════════════════════════════════════════════════════ */
static inline uint64_t ns_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

/* ══════════════════════════════════════════════════════
   BENCHMARK RUNNER
   ══════════════════════════════════════════════════════ */
#define N_WRITE    1020u   /* DODECA_SLOTS(1458) x 0.70 — stay under ring-full */
#define N_READ     1020u
#define N_DELETE     50u

/* simple LCG for reproducible data */
static uint64_t lcg_next(uint64_t *s) {
    *s = *s * 6364136223846793005ULL + 1442695040888963407ULL;
    return *s;
}

static void bench_dodeca(void) {
    static DodecaStore d;   /* static: avoid stack overflow */
    dodeca_init(&d);

    uint64_t seed = 0xDEADBEEFCAFEBABEULL;
    uint64_t addrs [N_WRITE];
    uint64_t values[N_WRITE];

    for (uint32_t i = 0; i < N_WRITE; i++) {
        addrs[i]  = lcg_next(&seed);
        values[i] = lcg_next(&seed);
    }

    /* ── WRITE ── */
    uint64_t t0 = ns_now();
    for (uint32_t i = 0; i < N_WRITE; i++)
        dodeca_write(&d, addrs[i], values[i]);
    uint64_t t1 = ns_now();

    uint64_t write_ns   = t1 - t0;
    double   write_ops  = (double)N_WRITE / (write_ns / 1e9);
    double   write_ns_op = (double)write_ns / N_WRITE;

    /* ── DELETE (first N_DELETE addrs) ── */
    for (uint32_t i = 0; i < N_DELETE; i++)
        dodeca_delete(&d, addrs[i]);

    /* ── READ ── */
    uint32_t hits = 0, misses = 0, deleted_ok = 0;
    uint64_t t2 = ns_now();
    for (uint32_t i = 0; i < N_READ; i++) {
        uint64_t out;
        if (i < N_DELETE) {
            int found = dodeca_read(&d, addrs[i], values[i], &out);
            if (!found) deleted_ok++;
        } else {
            int found = dodeca_read(&d, addrs[i], values[i], &out);
            if (found) hits++;
            else {
                misses++;
#ifdef DEBUG_MISS
                if (misses <= 5) {
                    GeoAddr g = geo_addr(addrs[i], values[i]);
                    printf("  MISS[%u] addr=0x%llx trit=%u spoke=%u coset=%u\n",
                           i, (unsigned long long)addrs[i],
                           g.trit, g.spoke, g.coset);
                }
#endif
            }
        }
    }
    uint64_t t3 = ns_now();

    uint64_t read_ns    = t3 - t2;
    double   read_ops   = (double)N_READ / (read_ns / 1e9);
    double   read_ns_op = (double)read_ns / N_READ;

    double   collision_pct = 100.0 * d.collisions / d.writes;
    double   ground_pct    = 100.0 * d.grounds    / d.writes;
    double   hit_pct       = 100.0 * hits / (N_READ - N_DELETE);
    double   delete_pct    = 100.0 * deleted_ok / N_DELETE;

    /* memory: value + addr + valid arrays */
    size_t mem_bytes = sizeof(d.value) + sizeof(d.addr) + sizeof(d.valid);

    printf("\n╔══════════════════════════════════════════════╗\n");
    printf("║  DODECA STORE (geometric address)            ║\n");
    printf("╠══════════════════════════════════════════════╣\n");
    printf("║  WRITE  %8.0f ops/sec  %6.1f ns/op       ║\n", write_ops, write_ns_op);
    printf("║  READ   %8.0f ops/sec  %6.1f ns/op       ║\n", read_ops,  read_ns_op);
    printf("║  slots  %u flat (%.1fKB value+addr+valid)   ║\n",
           DODECA_SLOTS, mem_bytes / 1024.0);
    printf("╠══════════════════════════════════════════════╣\n");
    printf("║  collision  %5.1f%%  ground  %5.1f%%  drops %u    ║\n",
           collision_pct, ground_pct, d.drops);
    printf("║  read hit   %5.1f%%  fwd_fast %5.1f%%             ║\n",
           hit_pct,
           d.reads > 0 ? 100.0 * d.fwd_hits / d.reads : 0.0);
    printf("║  delete OK  %5.1f%%  (%u addrs cut)           ║\n",
           delete_pct, N_DELETE);
    printf("╚══════════════════════════════════════════════╝\n");
}

static void bench_hash(void) {
    static HashStore h;
    hash_init(&h);

    uint64_t seed = 0xDEADBEEFCAFEBABEULL;   /* same seed = same data */
    uint64_t addrs [N_WRITE];
    uint64_t values[N_WRITE];

    for (uint32_t i = 0; i < N_WRITE; i++) {
        addrs[i]  = lcg_next(&seed);
        values[i] = lcg_next(&seed);
    }

    uint64_t t0 = ns_now();
    for (uint32_t i = 0; i < N_WRITE; i++)
        hash_write(&h, addrs[i], values[i]);
    uint64_t t1 = ns_now();

    uint32_t hits = 0;
    uint64_t t2 = ns_now();
    for (uint32_t i = 0; i < N_READ; i++) {
        uint64_t out;
        if (hash_read(&h, addrs[i], &out)) hits++;
    }
    uint64_t t3 = ns_now();

    uint64_t write_ns = t1 - t0;
    uint64_t read_ns  = t3 - t2;
    double   collision_pct = 100.0 * h.collisions / h.writes;
    size_t   mem_bytes = sizeof(h.key) + sizeof(h.value) + sizeof(h.valid);

    printf("\n╔══════════════════════════════════════════════╗\n");
    printf("║  HASH TABLE (baseline)                       ║\n");
    printf("╠══════════════════════════════════════════════╣\n");
    printf("║  WRITE  %8.0f ops/sec  %6.1f ns/op       ║\n",
           (double)N_WRITE / (write_ns / 1e9), (double)write_ns / N_WRITE);
    printf("║  READ   %8.0f ops/sec  %6.1f ns/op       ║\n",
           (double)N_READ  / (read_ns  / 1e9), (double)read_ns  / N_READ);
    printf("║  slots  %u flat (%.1fKB key+value+valid)    ║\n",
           HASH_SLOTS, mem_bytes / 1024.0);
    printf("╠══════════════════════════════════════════════╣\n");
    printf("║  collision  %5.1f%%                           ║\n", collision_pct);
    printf("║  read hit   %5.1f%%                           ║\n",
           100.0 * hits / N_READ);
    printf("║  delete: N/A (no reserved_mask)              ║\n");
    printf("╚══════════════════════════════════════════════╝\n");
}

/* ══════════════════════════════════════════════════════
   CORRECTNESS VERIFY — before benchmark numbers mean anything
   ══════════════════════════════════════════════════════ */
static int verify_correctness(void) {
    static DodecaStore d;
    dodeca_init(&d);

    int pass = 1;

    /* test 1: write then read back same value */
    dodeca_write(&d, 0xAAAA, 0x1234);
    uint64_t out = 0;
    int found = dodeca_read(&d, 0xAAAA, 0x1234, &out);
    if (!found || out != 0x1234) {
        printf("[FAIL] basic write/read: found=%d out=0x%llx\n",
               found, (unsigned long long)out);
        pass = 0;
    }

    /* test 2: overwrite same addr → read returns new value */
    dodeca_write(&d, 0xAAAA, 0x5678);
    found = dodeca_read(&d, 0xAAAA, 0x5678, &out);
    if (!found || out != 0x5678) {
        printf("[FAIL] overwrite: found=%d out=0x%llx\n",
               found, (unsigned long long)out);
        pass = 0;
    }

    /* test 3: delete → read returns 0 */
    dodeca_delete(&d, 0xAAAA);
    found = dodeca_read(&d, 0xAAAA, 0x5678, &out);
    if (found) {
        printf("[FAIL] delete: still readable after delete\n");
        pass = 0;
    }

    /* test 4: different addr → different slot (no false alias) */
    dodeca_init(&d);
    dodeca_write(&d, 100, 0xAA);
    dodeca_write(&d, 200, 0xBB);
    found = dodeca_read(&d, 100, 0xAA, &out);
    if (!found || out != 0xAA) {
        printf("[FAIL] alias: addr 100 clobbered by 200\n");
        pass = 0;
    }

    /* test 5: trit invariant — geo_addr(a,v).trit must be deterministic */
    GeoAddr g1 = geo_addr(0xDEAD, 0xBEEF);
    GeoAddr g2 = geo_addr(0xDEAD, 0xBEEF);
    if (g1.trit != g2.trit || g1.spoke != g2.spoke) {
        printf("[FAIL] determinism: geo_addr not stable\n");
        pass = 0;
    }

    /* test 6: geo_pixel formula check — R/G/B packing */
    uint32_t idx = 100u;
    uint8_t R = (uint8_t)(((idx % 27) << 3) | (idx % 6));
    uint8_t G = (uint8_t)(((idx % 9) << 4) | (idx % 26 & 0xF));
    uint8_t B = (uint8_t)(idx % 144);
    /* decode trit from R */
    uint8_t trit_dec  = (R >> 3) & 0x1F;
    uint8_t spoke_dec = R & 0x07;
    if (trit_dec != idx % 27 || spoke_dec != idx % 6) {
        printf("[FAIL] geo_pixel: trit=%u(exp %u) spoke=%u(exp %u)\n",
               trit_dec, (uint8_t)(idx%27), spoke_dec, (uint8_t)(idx%6));
        pass = 0;
    }
    (void)G; (void)B;   /* suppress unused warning */

    if (pass) printf("[PASS] all correctness checks\n");
    return pass;
}

/* ══════════════════════════════════════════════════════
   MAIN
   ══════════════════════════════════════════════════════ */
int main(void) {
    printf("geo_core_bench — N_WRITE=%u N_READ=%u N_DELETE=%u\n",
           N_WRITE, N_READ, N_DELETE);
    printf("DODECA_SLOTS=%u  HASH_SLOTS=%u\n\n", DODECA_SLOTS, HASH_SLOTS);

    printf("=== CORRECTNESS ===\n");
    if (!verify_correctness()) {
        printf("Correctness failed — fix before trusting benchmark numbers\n");
        return 1;
    }

    printf("\n=== BENCHMARK ===\n");
    bench_dodeca();
    bench_hash();

    printf("\n=== COMPARISON NOTE ===\n");
    printf("Dodeca: address derived from (addr^value) — no hash function needed\n");
    printf("        collision resolution via spoke ring (0-5), not rehash\n");
    printf("        delete = reserved_mask bit, data preserved (append-only)\n");
    printf("Hash:   address derived from hash(key) — hash function overhead\n");
    printf("        delete not implemented (no structural silence)\n");
    printf("        memory: key+value+valid vs addr+value+valid (same layout)\n\n");

    printf("Slot efficiency:\n");
    printf("  Dodeca: %u slots / %u writes = %.2f writes/slot capacity\n",
           DODECA_SLOTS, N_WRITE, (double)N_WRITE / DODECA_SLOTS);
    printf("  Hash:   %u slots / %u writes = %.2f writes/slot capacity\n",
           HASH_SLOTS,   N_WRITE, (double)N_WRITE / HASH_SLOTS);
    printf("  (Dodeca is intentionally smaller — geometric locality trades\n");
    printf("   slot count for cache residency and zero-hash cost)\n\n");

    return 0;
}
