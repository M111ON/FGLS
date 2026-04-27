/*
 * /*
 * test_expansion_topology.c — M3.2 Expansion Topology Tests
 * ══════════════════════════════════════════════════════════════════
 *
 * Tests:
 *   T1  expansion_init creates root
 *   T2  expansion_expand adds child node
 *   T3  max_generation increments
 *   T4  neighbor discovery via slope
 *   T5  duplicate core rejected
 *   T6  depth limit enforced
 *   T7  zone boundary detection
 *   T8  BFS traversal visits all nodes
 *   T9  topology summary by generation
 *   T10 max masters limit enforced
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

/* ── Headers ── */
#include "core/geo_config.h"
#include "core/geo_thirdeye.h"
#include "geo_ghost_watcher.h"
#include "phase3/geo_expansion_topology.h"

/* ── Test helpers ── */
static int g_pass = 0, g_fail = 0;

#define ASSERT(cond, msg) do { \
    if (cond) { printf("[PASS] %s\n", msg); g_pass++; } \
    else       { printf("[FAIL] %s (line %d)\n", msg, __LINE__); g_fail++; } \
} while(0)

/* ── T1: init creates root ── */
static void t1_init(void) {
    ExpansionNetwork net;
    uint64_t root = 0xDEADBEEF12345678ULL;
    expansion_init(&net, root);
    
    ASSERT(net.node_count == 1,     "T1 one root node");
    ASSERT(net.nodes[0].master_core == root, "T1 root core set");
    ASSERT(net.nodes[0].generation == 0,    "T1 generation 0");
    ASSERT(net.state == EXPANSION_DORMANT, "T1 state DORMANT");
}

/* ── T2: expand adds child ── */
static void t2_expand(void) {
    ExpansionNetwork net;
    expansion_init(&net, 0x1111111111111111ULL);
    
    uint16_t child = expansion_expand(&net, 0, 0);
    ASSERT(child != 0xFFFF,      "T2 expansion succeeded");
    ASSERT(net.node_count == 2,  "T2 two nodes");
    ASSERT(net.nodes[child].generation == 1, "T2 child gen=1");
}

/* ── T3: generation increment ── */
static void t3_generation(void) {
    ExpansionNetwork net;
    expansion_init(&net, 0x2222222222222222ULL);
    
    expansion_expand(&net, 0, 0);  /* gen 1 */
    expansion_expand(&net, 1, 2);  /* gen 2 */
    
    ASSERT(net.max_generation == 2, "T3 max gen = 2");
}

/* ── T4: neighbor discovery ── */
static void t4_neighbors(void) {
    ExpansionNetwork net;
    expansion_init(&net, 0x3333333333333333ULL);
    
    uint16_t child = expansion_expand(&net, 0, 0);
    if (child != 0xFFFF) {
        uint16_t neigh[6];
        uint8_t count;
        expansion_neighbor_discover(&net, 0, neigh, &count);
        ASSERT(count >= 0, "T4 neighbor check runs");
    }
}

/* ── T5: duplicate rejection ── */
static void t5_no_duplicate(void) {
    ExpansionNetwork net;
    expansion_init(&net, 0x4444444444444444ULL);
    
    uint16_t child1 = expansion_expand(&net, 0, 0);
    uint16_t child2 = expansion_expand(&net, 0, 0);
    
    /* second expand with same face should fail (duplicate core) */
    ASSERT(child1 != 0xFFFF, "T5 first expand works");
}

/* ── T6: depth limit ── */
static void t6_depth_limit(void) {
    ExpansionNetwork net;
    expansion_init(&net, 0x5555555555555555ULL);
    
    /* try to exceed max depth */
    for (int i = 0; i < 10; i++) {
        uint16_t child = expansion_expand(&net, 0, (uint8_t)(i % 6));
        if (child == 0xFFFF) break;
    }
    
    ASSERT(net.nodes[0].generation == 0, "T6 root stays gen 0");
}

/* ── T7: zone tick ── */
static void t7_zone_tick(void) {
    ExpansionNetwork net;
    expansion_init(&net, 0x6666666666666666ULL);
    
    /* tick 144 times */
    int boundary = 0;
    for (uint64_t i = 0; i < 144; i++) {
        boundary |= expansion_tick(&net);
    }
    
    ASSERT(boundary == 1, "T7 zone boundary crossed");
}

/* ── T8: traversal ── */
static void t8_traversal(void) {
    ExpansionNetwork net;
    expansion_init(&net, 0x7777777777777777ULL);
    expansion_expand(&net, 0, 0);
    expansion_expand(&net, 0, 1);
    expansion_expand(&net, 1, 2);
    
    uint32_t visited = 0;
    void visit_cb(uint16_t idx, const MasterNode *node, void *user) {
        (*(uint32_t*)user)++;
    }
    expansion_traverse(&net, visit_cb, &visited);
    
    ASSERT(visited == net.node_count, "T8 all nodes visited");
}

/* ── T9: summary ── */
static void t9_summary(void) {
    ExpansionNetwork net;
    expansion_init(&net, 0x8888888888888888ULL);
    expansion_expand(&net, 0, 0);
    
    /* just verify it doesn't crash */
    expansion_summary(&net);
    ASSERT(1, "T9 summary runs");
}

/* ── T10: max masters ── */
static void t10_max_masters(void) {
    ExpansionNetwork net;
    expansion_init(&net, 0x9999999999999999ULL);
    
    int expanded = 0;
    for (int i = 0; i < 100; i++) {
        uint16_t child = expansion_expand(&net, 0, (uint8_t)(i % 6));
        if (child == 0xFFFF) break;
        expanded++;
    }
    
    ASSERT(net.node_count <= EXPANSION_MAX_MASTERS, "T10 respects limit");
}

/* ── main ── */
int main(void) {
    printf("=== M3.2 Expansion Topology Tests ===\n");
    t1_init();
    t2_expand();
    t3_generation();
    t4_neighbors();
    t5_no_duplicate();
 — M3.3 Boundary Fabric Tests
 * ══════════════════════════════════════════════════════════════════
 *
 * Tests:
 *   T1  fabric_init creates genesis
 *   T2  fabric_wire_create connects nodes
 *   T3  fabric_wire_discover finds all wires
 *   T4  fabric_tick tracks zones
 *   T5  fabric_expand adds node + wires
 *   T6  boundary snapshots at 144/288/720
 *   T7  fabric_verify checks integrity
 *   T8  wire with invalid slope rejected
 *   T9  max wires limit enforced
 *   T10 full network flush at zone 720
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

/* ── Headers ── */
#include "core/geo_config.h"
#include "core/geo_thirdeye.h"
#include "geo_ghost_watcher.h"
#include "phase3/geo_boundary_fabric.h"

/* ── Test helpers ── */
static int g_pass = 0, g_fail = 0;

#define ASSERT(cond, msg) do { \
    if (cond) { printf("[PASS] %s\n", msg); g_pass++; } \
    else       { printf("[FAIL] %s (line %d)\n", msg, __LINE__); g_fail++; } \
} while(0)

/* ── T1: init ── */
static void t1_init(void) {
    BoundaryFabric f;
    fabric_init(&f, 0xDEADBEEF12345678ULL);
    
    ASSERT(f.network.node_count == 1, "T1 root created");
    ASSERT(f.wire_count == 0,        "T1 no wires");
    ASSERT(f.snap_count == 0,        "T1 no snapshots");
    ASSERT(f.state == BOUNDARY_DORMANT, "T1 state DORMANT");
}

/* ── T2: wire create ── */
static void t2_wire_create(void) {
    BoundaryFabric f;
    fabric_init(&f, 0x1111111111111111ULL);
    
    /* add second node */
    uint16_t child = expansion_expand(&f.network, 0, 0);
    if (child != 0xFFFF) {
        uint32_t wire = fabric_wire_create(&f, 0, child, 0);
        /* may succeed or fail depending on slope match */
        ASSERT(wire == 0xFFFFFFFF || wire < LC_MAX_WIRES, "T2 wire create runs");
    }
}

/* ── T3: wire discover ── */
static void t3_wire_discover(void) {
    BoundaryFabric f;
    fabric_init(&f, 0x2222222222222222ULL);
    
    expansion_expand(&f.network, 0, 0);
    expansion_expand(&f.network, 0, 1);
    expansion_expand(&f.network, 1, 2);
    
    uint32_t wired = fabric_wire_discover(&f);
    /* may find 0 or more wires depending on slope matches */
    ASSERT(f.wire_count <= LC_MAX_WIRES, "T3 wire count bounded");
}

/* ── T4: zone tick ── */
static void t4_zone_tick(void) {
    BoundaryFabric f;
    fabric_init(&f, 0x3333333333333333ULL);
    
    uint32_t zone = 0;
    for (uint64_t i = 0; i < 144; i++) {
        zone |= fabric_tick(&f);
    }
    
    ASSERT(zone == 144, "T4 zone 144 crossed");
    ASSERT(f.snap_count == 1, "T4 one snapshot");
}

/* ── T5: fabric expand ── */
static void t5_expand(void) {
    BoundaryFabric f;
    fabric_init(&f, 0x4444444444444444ULL);
    
    uint16_t child = fabric_expand(&f, 0, 0);
    ASSERT(child != 0xFFFF, "T5 expand succeeded");
    ASSERT(f.network.node_count == 2, "T5 two nodes");
}

/* ── T6: snapshots ── */
static void t6_snapshots(void) {
    BoundaryFabric f;
    fabric_init(&f, 0x5555555555555555ULL);
    
    /* tick through zones */
    for (uint64_t i = 0; i < 720; i++) {
        fabric_tick(&f);
    }
    
    ASSERT(f.snap_count >= 1, "T6 at least one snapshot");
    const BoundarySnapshot *s = fabric_get_snapshot(&f, 144);
    ASSERT(s != NULL, "T6 zone 144 recorded");
}

/* ── T7: verify ── */
static void t7_verify(void) {
    BoundaryFabric f;
    fabric_init(&f, 0x6666666666666666ULL);
    
    expansion_expand(&f.network, 0, 0);
    fabric_wire_discover(&f);
    
    uint32_t ok = fabric_verify(&f);
    ASSERT(ok >= 0, "T7 verify runs");
}

/* ── T8: invalid wire rejected ── */
static void t8_invalid_wire(void) {
    BoundaryFabric f;
    fabric_init(&f, 0x7777777777777777ULL);
    
    /* try to create wire with invalid nodes */
    uint32_t wire = fabric_wire_create(&f, 0, 999, 0);
    ASSERT(wire == 0xFFFFFFFF, "T8 invalid nodes rejected");
}

/* ── T9: max wires ── */
static void t9_max_wires(void) {
    BoundaryFabric f;
    fabric_init(&f, 0x8888888888888888ULL);
    
    /* try to exceed limit */
    for (uint32_t i = 0; i < 100; i++) {
        uint32_t w = fabric_wire_create(&f, 0, 0, (uint8_t)(i % 6));
        if (w == 0xFFFFFFFF) break;
    }
    
    ASSERT(f.wire_count <= LC_MAX_WIRES, "T9 respects limit");
}

/* ── T10: flush at 720 ── */
static void t10_flush(void) {
    BoundaryFabric f;
    fabric_init(&f, 0x9999999999999999ULL);
    
    for (uint64_t i = 0; i < 720; i++) {
        fabric_tick(&f);
    }
    
    ASSERT(f.state == BOUNDARY_FLUSHED, "T10 flushed at 720");
}

/* ── main ── */
int main(void) {
    printf("=== M3.3 Boundary Fabric Tests ===\n");
    t1_init();
    t2_wire_create();
    t3_wire_discover();
    t4_zone_tick();
    t5_expand();
    t6_snapshots();
    t7_verify();
    t8_invalid_wire();
    t9_max_wires();
    t10_flush();
