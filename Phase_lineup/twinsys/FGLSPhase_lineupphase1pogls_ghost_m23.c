#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ── shared primitives ─────────────────────────────────────── */
static inline uint64_t mix64(uint64_t x) {
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27; x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return x;
}
static inline uint64_t ghost_slope(uint64_t core, uint8_t face) {
    return mix64(core ^ ((uint64_t)face * 0x9E3779B185EBCA87ULL));
}
static inline uint64_t apex_derive_child(uint64_t parent, uint64_t apex_pat, uint8_t depth) {
    uint64_t d = (uint64_t)depth * 0x9E3779B185EBCA87ULL;
    return mix64(parent ^ apex_pat ^ d);
}
static inline int is_complement_pair(uint8_t a, uint8_t b) {
    return (a < 6 && b < 6 && ((a + 3) % 6 == b || (b + 3) % 6 == a));
}
static inline int apex_check(uint64_t c0, uint64_t c1) {
    uint64_t xr = c0 ^ c1;
    int pc = __builtin_popcountll(xr);
    return (pc >= 15 && pc <= 20) || (pc >= 44 && pc <= 49);  /* Fibo harmonic band */
}

/* ── M2.3: Blueprint Node ──────────────────────────────────── */
#define MAX_NODES   256
#define MAX_DEPTH   4
#define MAX_CHILDREN 6   /* up to 6 apex events per node (face pairs) */

typedef struct BpNode {
    uint64_t core;
    uint8_t  depth;
    uint8_t  child_count;
    uint16_t children[MAX_CHILDREN];   /* index into fabric table */
    uint16_t parent_idx;
    uint8_t  apex_face_a;
    uint8_t  apex_face_b;
} BpNode;

typedef struct Blueprint {
    BpNode   nodes[MAX_NODES];
    uint16_t count;
    uint16_t root;
} Blueprint;

/* spawn: try all face pairs → collect apex events → create children */
static int bp_spawn(Blueprint *bp, uint16_t node_idx) {
    BpNode *parent = &bp->nodes[node_idx];
    if (parent->depth >= MAX_DEPTH - 1) return 0;   /* depth limit */

    int spawned = 0;
    for (uint8_t fa = 0; fa < 6 && spawned < MAX_CHILDREN; fa++) {
        for (uint8_t fb = fa+1; fb < 6 && spawned < MAX_CHILDREN; fb++) {
            if (is_complement_pair(fa, fb)) continue;   /* skip entanglement */
            uint64_t c0 = ghost_slope(parent->core, fa);
            uint64_t c1 = ghost_slope(parent->core, fb);
            if (!apex_check(c0, c1)) continue;

            if (bp->count >= MAX_NODES) break;

            uint64_t apex_pat = c0 ^ c1;
            uint64_t child_core = apex_derive_child(parent->core, apex_pat, parent->depth + 1);

            /* dedup: skip if child_core already exists */
            int dup = 0;
            for (int k = 0; k < bp->count; k++)
                if (bp->nodes[k].core == child_core) { dup=1; break; }
            if (dup) continue;

            uint16_t ci = bp->count++;
            BpNode *child = &bp->nodes[ci];
            child->core        = child_core;
            child->depth       = parent->depth + 1;
            child->child_count = 0;
            child->parent_idx  = node_idx;
            child->apex_face_a = fa;
            child->apex_face_b = fb;

            parent->children[parent->child_count++] = ci;
            spawned++;
        }
    }
    return spawned;
}

/* lazy expand: BFS up to MAX_DEPTH */
static void bp_expand(Blueprint *bp) {
    uint16_t queue[MAX_NODES];
    int head=0, tail=0;
    queue[tail++] = bp->root;

    while (head < tail) {
        uint16_t idx = queue[head++];
        int n = bp_spawn(bp, idx);
        BpNode *nd = &bp->nodes[idx];
        for (int i = nd->child_count - n; i < nd->child_count; i++)
            if (tail < MAX_NODES) queue[tail++] = nd->children[i];
    }
}

/* ── Tests T15–T19 ─────────────────────────────────────────── */
static uint64_t find_fertile_root(void) {
    for (uint64_t s = 1; s < 1000000; s++) {
        uint64_t c = mix64(s);
        for (uint8_t a=0; a<6; a++) for (uint8_t b=a+1; b<6; b++) {
            if (is_complement_pair(a,b)) continue;
            if (apex_check(ghost_slope(c,a), ghost_slope(c,b))) return c;
        }
    }
    return 0;
}

static int test_T15_fabric_grows(void) {
    Blueprint bp = {0};
    bp.nodes[0].core = find_fertile_root();
    bp.nodes[0].depth = 0;
    bp.count = 1; bp.root = 0;
    bp_expand(&bp);
    return bp.count > 1;
}

static int test_T16_depth_limit(void) {
    Blueprint bp = {0};
    bp.nodes[0].core = find_fertile_root();
    bp.nodes[0].depth = 0;
    bp.count = 1; bp.root = 0;
    bp_expand(&bp);
    for (int i=0; i<bp.count; i++)
        if (bp.nodes[i].depth >= MAX_DEPTH) return 0;
    return 1;
}

static int test_T17_no_complement_apex(void) {
    /* complement pairs must never produce apex */
    uint64_t core = 0xABCDEF1234567890ULL;
    for (uint8_t f=0; f<3; f++) {
        uint64_t c0 = ghost_slope(core, f);
        uint64_t c1 = ghost_slope(core, f+3);
        if (apex_check(c0, c1)) return 0;
    }
    return 1;
}

static int test_T18_child_unique(void) {
    Blueprint bp = {0};
    bp.nodes[0].core = find_fertile_root();
    bp.nodes[0].depth = 0;
    bp.count = 1; bp.root = 0;
    bp_expand(&bp);
    for (int i=0; i<bp.count; i++)
        for (int j=i+1; j<bp.count; j++)
            if (bp.nodes[i].core == bp.nodes[j].core) return 0;
    return 1;
}

static int test_T19_parent_links(void) {
    Blueprint bp = {0};
    bp.nodes[0].core = find_fertile_root();
    bp.nodes[0].depth = 0;
    bp.count = 1; bp.root = 0;
    bp_expand(&bp);
    /* every non-root node: depth == parent.depth+1 */
    for (int i=1; i<bp.count; i++) {
        BpNode *n = &bp.nodes[i];
        BpNode *p = &bp.nodes[n->parent_idx];
        if (n->depth != p->depth + 1) return 0;
    }
    return 1;
}

int main(void) {
    printf("=== M2.3 Blueprint Fabric ===\n");

    struct { const char *name; int(*fn)(void); } tests[] = {
        {"T15 fabric grows from root",      test_T15_fabric_grows},
        {"T16 depth limit enforced",        test_T16_depth_limit},
        {"T17 no complement apex",          test_T17_no_complement_apex},
        {"T18 all child cores unique",      test_T18_child_unique},
        {"T19 parent depth links correct",  test_T19_parent_links},
    };
    int pass=0, total=sizeof(tests)/sizeof(tests[0]);
    for (int i=0; i<total; i++) {
        int r = tests[i].fn();
        printf("[%s] %s\n", r?"PASS":"FAIL", tests[i].name);
        pass += r;
    }

    /* topology summary */
    Blueprint bp = {0};
    uint64_t fertile = find_fertile_root();
    bp.nodes[0].core = fertile;
    bp.nodes[0].depth = 0;
    bp.count = 1; bp.root = 0;
    bp_expand(&bp);
    printf("\nFabric topology (fertile root=0x%016lX):\n", fertile);
    int depth_count[MAX_DEPTH] = {0};
    for (int i=0; i<bp.count; i++) depth_count[bp.nodes[i].depth]++;
    for (int d=0; d<MAX_DEPTH; d++)
        if (depth_count[d]) printf("  depth %d: %d nodes\n", d, depth_count[d]);
    printf("  total nodes: %d\n", bp.count);

    printf("\n%d/%d PASS\n", pass, total);
    return pass == total ? 0 : 1;
}
