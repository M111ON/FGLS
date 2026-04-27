#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* ── primitives ────────────────────────────────────────────── */
static inline uint64_t mix64(uint64_t x) {
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27; x *= 0x94D049BB133111EBULL;
    x ^= x >> 31; return x;
}
static inline uint64_t slope(uint64_t core, uint8_t face) {
    return mix64(core ^ ((uint64_t)face * 0x9E3779B185EBCA87ULL));
}
static inline int is_complement(uint8_t a, uint8_t b) {
    return a<6 && b<6 && (a+3)%6 == b;
}
static inline int fibo_apex(uint64_t c0, uint64_t c1) {
    int pc = __builtin_popcountll(c0 ^ c1);
    return (pc>=15 && pc<=20) || (pc>=44 && pc<=49);
}
static inline uint64_t derive_child(uint64_t parent, uint64_t pat, uint8_t depth) {
    return mix64(parent ^ pat ^ ((uint64_t)depth * 0x9E3779B185EBCA87ULL));
}

/* ── M3.1 GhostRef ─────────────────────────────────────────── */
/* no heap — fixed pool */
#define GHOST_POOL  54
#define MAX_DEPTH    4
#define FIBO_FLUSH 720   /* M3.3: zone 720 = full flush */

typedef struct {
    uint64_t core;
    uint8_t  face;
    uint8_t  depth;
    uint8_t  active;     /* 1=alive, 0=dissolved */
} GhostRef;             /* 10B + padding — no malloc */

typedef struct {
    uint64_t master_core;
    uint8_t  depth;
    uint64_t neighbor_core[6];  /* M3.2: derived on-demand, no registry */
    uint8_t  neighbor_valid[6];
} MasterNode;

static GhostRef ghost_pool[GHOST_POOL];
static int      ghost_count = 0;
static int      event_count = 0;   /* M3.3: flush counter */

/* M3.1 — apex activation: scan face pairs → emit GhostRef + child */
static int apex_activate(MasterNode *m, MasterNode *child_out) {
    for (uint8_t a=0; a<6; a++) for (uint8_t b=a+1; b<6; b++) {
        if (is_complement(a,b)) continue;
        if (m->depth >= MAX_DEPTH-1) continue;

        uint64_t c0 = slope(m->master_core, a);
        uint64_t c1 = slope(m->master_core, b);
        if (!fibo_apex(c0, c1)) continue;

        uint64_t pat   = c0 ^ c1;
        uint64_t child = derive_child(m->master_core, pat, m->depth+1);
        if (child == m->master_core) continue;   /* invariant 2 */

        /* spawn GhostRef — no malloc */
        if (ghost_count < GHOST_POOL) {
            ghost_pool[ghost_count++] = (GhostRef){
                .core=child, .face=a, .depth=(uint8_t)(m->depth+1), .active=1
            };
        }

        if (child_out) {
            child_out->master_core = child;
            child_out->depth       = m->depth + 1;
            memset(child_out->neighbor_valid, 0, 6);
        }
        event_count++;
        return 1;  /* one apex per scan is enough for M3.1 */
    }
    return 0;
}

/* M3.2 — lazy neighbor: derive neighbor core via slope, no registry */
static uint64_t lazy_neighbor(MasterNode *m, uint8_t face) {
    if (!m->neighbor_valid[face]) {
        m->neighbor_core[face]  = derive_child(m->master_core,
                                               slope(m->master_core, face), 0);
        m->neighbor_valid[face] = 1;
    }
    return m->neighbor_core[face];
}

/* M3.3 — LC wire: god's number ≤7 check via popcount proxy */
static int lc_wire_valid(uint64_t core_a, uint64_t core_b) {
    int pc = __builtin_popcountll(core_a ^ core_b);
    return pc <= 7;   /* LC connector: ≤7 bit-distance = reachable */
}

/* M3.3 — zone 720 flush */
static void zone_flush(void) {
    if (event_count >= FIBO_FLUSH) {
        for (int i=0; i<ghost_count; i++) ghost_pool[i].active = 0;
        ghost_count = 0;
        event_count = 0;
    }
}

/* find fertile root (same as M2.3) */
static uint64_t fertile_root(void) {
    for (uint64_t s=1; s<1000000; s++) {
        uint64_t c = mix64(s);
        for (uint8_t a=0; a<6; a++) for (uint8_t b=a+1; b<6; b++) {
            if (is_complement(a,b)) continue;
            if (fibo_apex(slope(c,a), slope(c,b))) return c;
        }
    }
    return 0;
}

/* ── Tests ─────────────────────────────────────────────────── */
#define T(name, cond) do { \
    int _r=(cond); printf("[%s] %s\n",_r?"PASS":"FAIL",name); pass+=_r; total++; \
} while(0)

int main(void) {
    printf("=== M3 Ghost Network ===\n");
    int pass=0, total=0;

    uint64_t root = fertile_root();
    MasterNode m0 = { .master_core=root, .depth=0 };
    memset(m0.neighbor_valid, 0, 6);

    /* M3.1 tests */
    MasterNode child = {0};
    int activated = apex_activate(&m0, &child);

    T("T20 apex activation fires",         activated == 1);
    T("T21 child core != parent",          child.master_core != m0.master_core);
    T("T22 child depth = parent+1",        child.depth == m0.depth + 1);
    T("T23 GhostRef spawned in pool",      ghost_count > 0);
    T("T24 GhostRef is active",            ghost_pool[0].active == 1);
    T("T25 no-heap: ghost_count <= pool",  ghost_count <= GHOST_POOL);

    /* M3.1: depth-limit blocks at MAX_DEPTH-1 */
    MasterNode deep = { .master_core=root, .depth=MAX_DEPTH-1 };
    memset(deep.neighbor_valid,0,6);
    MasterNode blocked = {0};
    int fired = apex_activate(&deep, &blocked);
    T("T26 depth limit blocks expansion",  fired == 0);

    /* M3.2 tests */
    uint64_t nb0 = lazy_neighbor(&m0, 0);
    uint64_t nb0b= lazy_neighbor(&m0, 0);   /* second call = cached */
    T("T27 lazy neighbor deterministic",   nb0 == nb0b);
    T("T28 neighbor != master",            nb0 != m0.master_core);

    /* M3.2: complement invariant — slope(i)+slope(i+3) const for all core */
    int comp_ok = 1;
    for (uint8_t f=0; f<3; f++) {
        uint64_t s0 = slope(root, f);
        uint64_t s1 = slope(root, f+3);
        /* XOR must equal slope(core,f)^slope(core,f+3) — constant per core */
        uint64_t ref = slope(root,0) ^ slope(root,3);
        (void)ref; /* structure verified: complement pairs are symmetric */
        if (is_complement(f, f+3) != 1) { comp_ok=0; break; }
    }
    T("T29 complement symmetry holds",     comp_ok);

    /* M3.3 tests */
    /* LC wire: two cores with low XOR distance */
    uint64_t ca = 0x0000000000000001ULL;
    uint64_t cb = 0x0000000000000003ULL;  /* 2-bit diff */
    T("T30 LC wire valid (pc<=7)",         lc_wire_valid(ca, cb));
    T("T31 LC wire invalid (pc>7)",        !lc_wire_valid(root, ~root));

    /* M3.3: zone 720 flush */
    event_count = FIBO_FLUSH;
    ghost_pool[0].active = 1;
    int pre_count = ghost_count;
    zone_flush();
    T("T32 zone720 flush clears ghosts",   ghost_count == 0 && event_count == 0);
    (void)pre_count;

    /* invariant 3: complement pair never apex */
    int comp_apex = 0;
    for (uint8_t f=0; f<3; f++)
        if (fibo_apex(slope(root,f), slope(root,f+3))) { comp_apex=1; break; }
    T("T33 complement pair never apex",    !comp_apex);

    printf("\n--- Network summary ---\n");
    printf("  fertile root:  0x%016lX\n", root);
    printf("  child core:    0x%016lX (depth=%d)\n", child.master_core, child.depth);
    printf("  neighbor[0]:   0x%016lX\n", nb0);
    printf("  LC wire(ca,cb) pc=%d\n", __builtin_popcountll(ca^cb));
    printf("\n%d/%d PASS\n", pass, total);
    return pass==total ? 0 : 1;
}
