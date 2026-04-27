/*
 * geo_shape_bench.c — File-as-Shape Placement Benchmark
 * ═══════════════════════════════════════════════════════
 *
 * Models the real system: a file has geometry (shape) derived from its
 * metadata. Placement = map shape → slot. Retrieval = give shape → get file.
 * No mutation. No overwrite. Delete = structural silence (reserved_mask).
 *
 * Shape of a file is derived from:
 *   size_bucket  → trit   (0-26)   how big is it?
 *   type_hash    → spoke  (0-5)    what kind is it?
 *   time_coset   → coset  (0-8)    when was it created?
 *   name_letter  → letter (0-25)   what is its name initial?
 *   content_fibo → fibo   (0-143)  what is its content fingerprint?
 *
 * The address of a file IS its shape. Two files with same shape → same slot.
 * This is content-addressable by geometry, not by hash.
 *
 * compile: gcc -O2 -o shape_bench geo_shape_bench.c && ./shape_bench
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* ══════════════════════════════════════════
   SACRED NUMBERS — do not change
   ══════════════════════════════════════════ */
#define TRIT_MOD    27u    /* 3^3 */
#define SPOKE_MOD    6u    /* frustum faces */
#define COSET_MOD    9u    /* 3^2 */
#define LETTER_MOD  26u    /* A-Z */
#define FIBO_MOD   144u    /* F(12) */

/* slot space: trit × spoke × coset = 1,458
   each slot holds one FileShape (the file's geometric identity)
   and a payload pointer (simulated as uint64_t fingerprint)     */
#define SHAPE_SLOTS  (TRIT_MOD * SPOKE_MOD * COSET_MOD)   /* 1,458 */

/* ══════════════════════════════════════════
   FILE METADATA → SHAPE
   Real inputs: filename, size, type, mtime, first 8 bytes of content
   ══════════════════════════════════════════ */
typedef struct {
    uint8_t  trit;    /* size bucket:  0-26  */
    uint8_t  spoke;   /* type family:  0-5   */
    uint8_t  coset;   /* time window:  0-8   */
    uint8_t  letter;  /* name initial: 0-25  */
    uint8_t  fibo;    /* content gear: 0-143 */
} FileShape;

typedef struct {
    uint64_t size;        /* file size in bytes    */
    uint8_t  type;        /* 0=bin,1=text,2=img... */
    uint64_t mtime;       /* unix timestamp        */
    uint8_t  name_first;  /* first char of name    */
    uint64_t content_sig; /* first 8 bytes XOR'd   */
} FileMeta;

/* derive geometric shape from file metadata — pure arithmetic, no hash */
static inline FileShape shape_from_meta(const FileMeta *m) {
    /* size → trit: log2 bucketing — files 1B..4GB map into 27 buckets
       each bucket spans a doubly-narrowing frustum (compression metaphor) */
    uint8_t  sz_bits = 0;
    uint64_t sz = m->size > 0 ? m->size : 1;
    while (sz > 1) { sz >>= 1; sz_bits++; }   /* log2(size) 0..63 */
    /* mix with content_sig low bits to separate files of same size */
    uint64_t trit_seed = (uint64_t)sz_bits * 3 + (m->content_sig & 0x3);
    /* mix bits for better spread */
    trit_seed ^= trit_seed >> 5;

    /* type → spoke: 6 families (binary/text/image/audio/video/data) */
    uint8_t spoke_seed = m->type % SPOKE_MOD;

    /* mtime → coset: time window (hour of day → 0..8)
       coset = (hour) % 9 groups files by when they were created */
    uint8_t coset_seed = (uint8_t)((m->mtime / 3600) % COSET_MOD);

    /* name_first → letter: A-Z (case folded) */
    uint8_t ltr = (m->name_first >= 'a' && m->name_first <= 'z')
                  ? (uint8_t)(m->name_first - 'a')
                  : (m->name_first >= 'A' && m->name_first <= 'Z')
                  ? (uint8_t)(m->name_first - 'A')
                  : (uint8_t)(m->name_first % LETTER_MOD);

    /* content_sig → fibo: gear position from content fingerprint */
    uint8_t fibo_seed = (uint8_t)(m->content_sig % FIBO_MOD);

    FileShape s;
    s.trit   = (uint8_t)(trit_seed  % TRIT_MOD);
    s.spoke  = (uint8_t)(spoke_seed % SPOKE_MOD);
    s.coset  = (uint8_t)(coset_seed % COSET_MOD);
    s.letter = ltr % LETTER_MOD;
    s.fibo   = fibo_seed;
    return s;
}

/* slot index from shape */
static inline uint32_t shape_slot(const FileShape *s) {
    return (uint32_t)s->trit  * (SPOKE_MOD * COSET_MOD)
         + (uint32_t)s->spoke * COSET_MOD
         + (uint32_t)s->coset;
}

/* ══════════════════════════════════════════
   SHAPE STORE — flat array, no malloc
   One slot = one "geometric position" in the file system
   Multiple files can share a slot (same shape = same position)
   → linked list per slot (max CHAIN_MAX, no heap: static pool)
   ══════════════════════════════════════════ */
#define CHAIN_MAX  8u   /* max files per shape slot (spoke ring depth) */

typedef struct {
    uint64_t  content_sig;   /* file fingerprint (simulates payload ptr) */
    FileMeta  meta;          /* full metadata for retrieval verify        */
    uint8_t   valid;         /* 0=empty 1=live 3=deleted(tombstone)       */
} ShapeEntry;

typedef struct {
    ShapeEntry chain[CHAIN_MAX];  /* ring buffer per slot */
    uint8_t    count;             /* live entries in slot */
    uint8_t    head;              /* next write position  */
    uint8_t    reserved;          /* 1 = coset deleted    */
} ShapeSlot;

typedef struct {
    ShapeSlot  slots[SHAPE_SLOTS];
    uint16_t   reserved_mask;    /* per-coset delete bitmask */
    /* stats */
    uint32_t   placed;
    uint32_t   retrieved;
    uint32_t   shape_hits;       /* retrieved from slot directly   */
    uint32_t   shape_miss;       /* shape match but content differ */
    uint32_t   deleted;
    uint32_t   slot_full;        /* chain overflow (CHAIN_MAX hit) */
} ShapeStore;

static inline void shape_store_init(ShapeStore *st) {
    memset(st, 0, sizeof(*st));
}

/* ── place: put file into its geometric slot ── */
static inline int shape_place(ShapeStore *st, const FileMeta *m) {
    FileShape s   = shape_from_meta(m);
    uint32_t  idx = shape_slot(&s);

    /* coset deleted = structural silence */
    if (st->reserved_mask & (1u << s.coset)) return -1;

    ShapeSlot *sl = &st->slots[idx];
    if (sl->reserved) return -1;

    /* find empty or tombstone slot in chain */
    for (uint8_t i = 0; i < CHAIN_MAX; i++) {
        uint8_t pos = (sl->head + i) % CHAIN_MAX;
        ShapeEntry *e = &sl->chain[pos];
        if (e->valid == 0 || e->valid == 3) {
            e->content_sig = m->content_sig;
            e->meta        = *m;
            e->valid       = 1;
            if (e->valid != 1) e->valid = 1;
            sl->count++;
            sl->head = (pos + 1) % CHAIN_MAX;
            st->placed++;
            return 0;
        }
    }
    st->slot_full++;
    return -2;   /* slot full — chain overflow */
}

/* ── retrieve: give shape fields → get matching file ──
   "I know it was a ~1MB text file created this morning starting with 'r'"
   → retrieve without knowing filename or exact path                        */
typedef struct {
    uint8_t  trit;     /* 0xFF = wildcard */
    uint8_t  spoke;
    uint8_t  coset;
    uint8_t  letter;   /* 0xFF = wildcard */
} ShapeQuery;

static inline int shape_retrieve(ShapeStore *st, const ShapeQuery *q,
                                  uint64_t *sig_out) {
    /* build partial shape — wildcards expand search across axis */
    uint8_t t_start = (q->trit  == 0xFF) ? 0 : q->trit;
    uint8_t t_end   = (q->trit  == 0xFF) ? TRIT_MOD  : (uint8_t)(q->trit  + 1);
    uint8_t l_any   = (q->letter == 0xFF);

    st->retrieved++;

    for (uint8_t t = t_start; t < t_end; t++) {
        uint32_t  idx = (uint32_t)t * (SPOKE_MOD * COSET_MOD)
                      + (uint32_t)q->spoke * COSET_MOD
                      + (uint32_t)q->coset;
        if (st->reserved_mask & (1u << q->coset)) continue;

        ShapeSlot *sl = &st->slots[idx];
        for (uint8_t i = 0; i < CHAIN_MAX; i++) {
            ShapeEntry *e = &sl->chain[i];
            if (e->valid != 1) continue;
            /* letter match */
            uint8_t name_ltr = (e->meta.name_first >= 'a')
                               ? (uint8_t)(e->meta.name_first - 'a')
                               : (uint8_t)(e->meta.name_first - 'A');
            if (!l_any && (name_ltr % LETTER_MOD) != q->letter) continue;
            *sig_out = e->content_sig;
            st->shape_hits++;
            return 1;
        }
    }
    st->shape_miss++;
    return 0;
}

/* ── delete: structural silence on coset ── */
static inline void shape_delete_coset(ShapeStore *st, uint8_t coset) {
    st->reserved_mask |= (uint16_t)(1u << coset);
    /* tombstone all entries in deleted coset */
    for (uint8_t t = 0; t < TRIT_MOD; t++) {
        for (uint8_t sp = 0; sp < SPOKE_MOD; sp++) {
            uint32_t idx = (uint32_t)t * (SPOKE_MOD * COSET_MOD)
                         + (uint32_t)sp * COSET_MOD + coset;
            ShapeSlot *sl = &st->slots[idx];
            sl->reserved = 1;
            for (uint8_t i = 0; i < CHAIN_MAX; i++)
                if (sl->chain[i].valid == 1) sl->chain[i].valid = 3;
            sl->count = 0;
        }
    }
    st->deleted++;
}

/* ══════════════════════════════════════════
   BASELINE: filesystem-style path lookup
   Simulates find-by-path: hash the full path string → bucket
   ══════════════════════════════════════════ */
#define PATH_BUCKETS  2048u

typedef struct {
    uint64_t path_hash;
    uint64_t content_sig;
    uint8_t  valid;
} PathEntry;

typedef struct {
    PathEntry entries[PATH_BUCKETS];
    uint32_t  placed;
    uint32_t  retrieved;
    uint32_t  collisions;
} PathStore;

static inline void path_store_init(PathStore *ps) { memset(ps, 0, sizeof(*ps)); }

static inline uint32_t path_hash(uint64_t h) {
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33;
    return (uint32_t)(h % PATH_BUCKETS);
}

static inline void path_place(PathStore *ps, uint64_t ph, uint64_t sig) {
    uint32_t b = path_hash(ph);
    for (uint32_t i = 0; i < PATH_BUCKETS; i++) {
        uint32_t bi = (b + i) % PATH_BUCKETS;
        if (!ps->entries[bi].valid) {
            ps->entries[bi].path_hash    = ph;
            ps->entries[bi].content_sig  = sig;
            ps->entries[bi].valid        = 1;
            ps->placed++;
            if (i > 0) ps->collisions++;
            return;
        }
    }
}

static inline int path_retrieve(PathStore *ps, uint64_t ph, uint64_t *sig_out) {
    uint32_t b = path_hash(ph);
    ps->retrieved++;
    for (uint32_t i = 0; i < PATH_BUCKETS; i++) {
        uint32_t bi = (b + i) % PATH_BUCKETS;
        if (!ps->entries[bi].valid) return 0;
        if (ps->entries[bi].path_hash == ph) {
            *sig_out = ps->entries[bi].content_sig;
            return 1;
        }
    }
    return 0;
}

/* ══════════════════════════════════════════
   TEST DATA GENERATOR
   Simulates realistic file metadata distribution
   ══════════════════════════════════════════ */
static uint64_t lcg(uint64_t *s) {
    *s = *s * 6364136223846793005ULL + 1442695040888963407ULL;
    return *s;
}

static const uint64_t FILE_SIZES[] = {
    512, 1024, 4096, 8192, 65536, 131072, 1048576, 4194304
};
static const uint8_t  FILE_TYPES[] = { 0,1,2,3,4,5 };  /* 6 types */
static const char     NAME_CHARS[] = "abcdefghijklmnopqrstuvwxyz";

static inline FileMeta gen_meta(uint64_t *seed) {
    FileMeta m;
    m.size        = FILE_SIZES[lcg(seed) % 8];
    m.type        = FILE_TYPES[lcg(seed) % 6];
    m.mtime       = lcg(seed) % (24 * 3600);   /* within one day */
    m.name_first  = NAME_CHARS[lcg(seed) % 26];
    m.content_sig = lcg(seed);
    return m;
}

/* ══════════════════════════════════════════
   CORRECTNESS
   ══════════════════════════════════════════ */
static int verify_correctness(void) {
    static ShapeStore st;
    shape_store_init(&st);
    int pass = 1;

    /* test 1: place then retrieve by exact shape */
    FileMeta m = { .size=4096, .type=1, .mtime=3600,
                   .name_first='r', .content_sig=0xDEADBEEF };
    shape_place(&st, &m);
    FileShape s = shape_from_meta(&m);
    ShapeQuery q = { s.trit, s.spoke, s.coset, s.letter };
    uint64_t out = 0;
    int found = shape_retrieve(&st, &q, &out);
    if (!found || out != 0xDEADBEEF) {
        printf("[FAIL] place/retrieve: found=%d sig=0x%llx\n",
               found, (unsigned long long)out);
        pass = 0;
    }

    /* test 2: shape determinism — same meta → same shape */
    FileShape s1 = shape_from_meta(&m);
    FileShape s2 = shape_from_meta(&m);
    if (s1.trit != s2.trit || s1.spoke != s2.spoke || s1.coset != s2.coset) {
        printf("[FAIL] shape not deterministic\n");
        pass = 0;
    }

    /* test 3: delete coset → retrieve returns 0 */
    shape_delete_coset(&st, s.coset);
    found = shape_retrieve(&st, &q, &out);
    if (found) {
        printf("[FAIL] delete: still retrievable after coset delete\n");
        pass = 0;
    }

    /* test 4: wildcard retrieve — spoke+coset match, any trit */
    shape_store_init(&st);
    FileMeta m2 = { .size=65536, .type=2, .mtime=7200,
                    .name_first='k', .content_sig=0xCAFEBABE };
    shape_place(&st, &m2);
    FileShape s3 = shape_from_meta(&m2);
    ShapeQuery qw = { 0xFF, s3.spoke, s3.coset, 0xFF };  /* wildcard trit+letter */
    found = shape_retrieve(&st, &qw, &out);
    if (!found) {
        printf("[FAIL] wildcard retrieve failed\n");
        pass = 0;
    }

    /* test 5: two files same shape → both in chain */
    shape_store_init(&st);
    FileMeta ma = { .size=4096, .type=1, .mtime=3600,
                    .name_first='a', .content_sig=0x1111 };
    FileMeta mb = { .size=4096, .type=1, .mtime=3600,
                    .name_first='a', .content_sig=0x2222 };
    shape_place(&st, &ma);
    shape_place(&st, &mb);
    FileShape sa = shape_from_meta(&ma);
    uint32_t  si = shape_slot(&sa);
    if (st.slots[si].count < 2) {
        printf("[FAIL] two same-shape files should both be in chain, count=%u\n",
               st.slots[si].count);
        pass = 0;
    }

    if (pass) printf("[PASS] all correctness checks\n");
    return pass;
}

/* ══════════════════════════════════════════
   TIMER
   ══════════════════════════════════════════ */
static inline uint64_t ns_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

/* ══════════════════════════════════════════
   BENCHMARK
   ══════════════════════════════════════════ */
#define N_FILES     1000u
#define N_QUERY     1000u
#define N_DEL_COSET    2u   /* delete 2 of 9 cosets */

static void bench_shape(void) {
    static ShapeStore st;
    shape_store_init(&st);

    uint64_t seed = 0xDEADBEEFCAFEBABEULL;
    FileMeta metas[N_FILES];
    for (uint32_t i = 0; i < N_FILES; i++)
        metas[i] = gen_meta(&seed);

    /* ── PLACE ── */
    uint64_t t0 = ns_now();
    uint32_t placed_ok = 0;
    for (uint32_t i = 0; i < N_FILES; i++)
        if (shape_place(&st, &metas[i]) == 0) placed_ok++;
    uint64_t t1 = ns_now();

    /* ── DELETE 2 cosets ── */
    shape_delete_coset(&st, 3);
    shape_delete_coset(&st, 7);

    /* ── RETRIEVE by shape (no path, no filename) ── */
    uint32_t hits = 0;
    uint64_t t2 = ns_now();
    for (uint32_t i = 0; i < N_QUERY; i++) {
        FileShape s = shape_from_meta(&metas[i]);
        /* skip deleted cosets */
        if (st.reserved_mask & (1u << s.coset)) continue;
        ShapeQuery q = { s.trit, s.spoke, s.coset, s.letter };
        uint64_t out;
        if (shape_retrieve(&st, &q, &out)) hits++;
    }
    uint64_t t3 = ns_now();

    uint64_t place_ns  = t1 - t0;
    uint64_t query_ns  = t3 - t2;
    double   place_ops = (double)N_FILES  / (place_ns / 1e9);
    double   query_ops = (double)N_QUERY  / (query_ns / 1e9);

    /* count live files after delete */
    uint32_t live = 0, deleted_count = 0;
    for (uint32_t i = 0; i < SHAPE_SLOTS; i++) {
        for (uint8_t j = 0; j < CHAIN_MAX; j++) {
            if (st.slots[i].chain[j].valid == 1) live++;
            if (st.slots[i].chain[j].valid == 3) deleted_count++;
        }
    }

    size_t mem = sizeof(ShapeStore);

    printf("\n╔══════════════════════════════════════════════════╗\n");
    printf("║  SHAPE STORE (geometry-addressed files)          ║\n");
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║  PLACE    %8.0f ops/sec  %6.1f ns/op         ║\n",
           place_ops, (double)place_ns / N_FILES);
    printf("║  RETRIEVE %8.0f ops/sec  %6.1f ns/op         ║\n",
           query_ops, (double)query_ns / N_QUERY);
    printf("║  memory   %.1fKB (%u slots × %u chain)          ║\n",
           mem / 1024.0, SHAPE_SLOTS, CHAIN_MAX);
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║  placed   %4u / %4u   slot_full %u              ║\n",
           placed_ok, N_FILES, st.slot_full);
    printf("║  live     %4u  deleted  %4u  (2 cosets cut)     ║\n",
           live, deleted_count);
    printf("║  retrieve hit %5.1f%%  (non-deleted range)        ║\n",
           100.0 * hits / (N_QUERY > 0 ? N_QUERY : 1));
    printf("║  wildcard available: spoke+coset → any trit/ltr  ║\n");
    printf("╚══════════════════════════════════════════════════╝\n");
}

static void bench_path(void) {
    static PathStore ps;
    path_store_init(&ps);

    uint64_t seed = 0xDEADBEEFCAFEBABEULL;
    uint64_t paths[N_FILES], sigs[N_FILES];
    for (uint32_t i = 0; i < N_FILES; i++) {
        paths[i] = lcg(&seed);   /* simulated path hash */
        sigs[i]  = lcg(&seed);
    }

    uint64_t t0 = ns_now();
    for (uint32_t i = 0; i < N_FILES; i++)
        path_place(&ps, paths[i], sigs[i]);
    uint64_t t1 = ns_now();

    uint32_t hits = 0;
    uint64_t t2 = ns_now();
    for (uint32_t i = 0; i < N_QUERY; i++) {
        uint64_t out;
        if (path_retrieve(&ps, paths[i], &out)) hits++;
    }
    uint64_t t3 = ns_now();

    printf("\n╔══════════════════════════════════════════════════╗\n");
    printf("║  PATH STORE (hash-addressed files, baseline)     ║\n");
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║  PLACE    %8.0f ops/sec  %6.1f ns/op         ║\n",
           (double)N_FILES  / ((t1-t0)/1e9), (double)(t1-t0)/N_FILES);
    printf("║  RETRIEVE %8.0f ops/sec  %6.1f ns/op         ║\n",
           (double)N_QUERY  / ((t3-t2)/1e9), (double)(t3-t2)/N_QUERY);
    printf("║  memory   %.1fKB (%u buckets)                    ║\n",
           sizeof(ps) / 1024.0, PATH_BUCKETS);
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║  retrieve hit %5.1f%%                             ║\n",
           100.0 * hits / N_QUERY);
    printf("║  collision    %5.1f%%                             ║\n",
           100.0 * ps.collisions / (ps.placed > 0 ? ps.placed : 1));
    printf("║  delete: requires full scan (no structural path) ║\n");
    printf("╚══════════════════════════════════════════════════╝\n");
}

/* ══════════════════════════════════════════
   MAIN
   ══════════════════════════════════════════ */
int main(void) {
    printf("geo_shape_bench — files=%u  queries=%u  del_cosets=%u\n",
           N_FILES, N_QUERY, N_DEL_COSET);
    printf("SHAPE_SLOTS=%u (%u trit × %u spoke × %u coset)  CHAIN=%u\n\n",
           SHAPE_SLOTS, TRIT_MOD, SPOKE_MOD, COSET_MOD, CHAIN_MAX);

    printf("=== CORRECTNESS ===\n");
    if (!verify_correctness()) return 1;

    printf("\n=== BENCHMARK ===\n");
    bench_shape();
    bench_path();

    printf("\n=== KEY DIFFERENCE ===\n");
    printf("Shape: retrieve by geometry  — 'find ~1MB text created this morning'\n");
    printf("       no filename needed    — shape IS the address\n");
    printf("       delete = coset silence — data preserved, path cut\n");
    printf("       wildcard query        — 'any trit, spoke=1, coset=3'\n\n");
    printf("Path:  retrieve by exact key — must know full path\n");
    printf("       delete = mark invalid — data gone, no audit trail\n");
    printf("       no partial query      — must know exact filename\n\n");
    printf("Shape store memory = %.1fKB  Path store = %.1fKB\n",
           sizeof(ShapeStore) / 1024.0, sizeof(PathStore) / 1024.0);
    return 0;
}
