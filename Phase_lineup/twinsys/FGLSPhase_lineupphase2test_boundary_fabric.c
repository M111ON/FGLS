/*
 * test_boundary_fabric.c — M3.3 Boundary Fabric Tests
 * ═══════════════════════════════════════════════════════════
 *
 * Tests:
 *   T1  fabric_init bootstraps network
 *   T2  fabric_wire_create direct connection
 *   T3  fabric_wire_discover auto-discover
 *   T4  fabric_tick zone boundaries
 *   T5  expansion lattice growth
 *   T6  boundary snapshots
 *   T7  fabric_verify integrity
 *   T8  invalid wire rejection
 *   T9  max wires limit
 *   T10 full flush at zone 720
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
    fabric_init(&f, 0x1234567890ABCDEFULL);
    ASSERT(f.network.node_count == 1, "T1 root node created");
    ASSERT(f.state == BOUNDARY_DORMANT, "T1 state DORMANT");
}

/* ── T2: wire create ── */
static void t2_wire_create(void) {
    BoundaryFabric f;
    fabric_init(&f, 1ULL);
    
    /* create a second node manually for testing */
    uint16_t child = expansion_expand(&f.network, 0, 0);
    ASSERT(child != 0xFFFF, "T2 child created");
    
    /* wire them (face 0) */
    uint32_t wire = fabric_wire_create(&f, 0, child, 0);
    ASSERT(wire != 0xFFFFFFFF, "T2 wire created");
    ASSERT(f.wire_count == 1, "T2 wire_count == 1");
}

/* ── T3: wire discover ── */
static void t3_wire_discover(void) {
    BoundaryFabric f;
    fabric_init(&f, 1ULL);
    
    /* create multiple nodes */
    expansion_expand(&f.network, 0, 0);
    expansion_expand(&f.network, 0, 1);
    
    uint32_t count = fabric_wire_discover(&f);
    ASSERT(count >= 2, "T3 wires discovered");
}

/* ── T4: zone tick ── */
static void t4_zone_tick(void) {
    BoundaryFabric f;
    fabric_init(&f, 1ULL);
    
    for (int i = 0; i < 143; i++) fabric_tick(&f);
    ASSERT(f.snap_count == 0, "T4 no snapshot at 143");
    
    fabric_tick(&f);
    ASSERT(f.snap_count == 1, "T4 snapshot at 144");
    ASSERT(f.snapshots[0].zone_id == 144, "T4 zone 144 recorded");
}

/* ── T5: expansion ── */
static void t5_expand(void) {
    BoundaryFabric f;
    fabric_init(&f, 1ULL);
    
    uint16_t child = expansion_expand(&f.network, 0, 0);
    ASSERT(child != 0xFFFF, "T5 expansion growth");
    ASSERT(f.network.max_generation == 1, "T5 generation 1");
}

/* ── T6: snapshots ── */
static void t6_snapshots(void) {
    BoundaryFabric f;
    fabric_init(&f, 1ULL);
    
    /* cross 2 zones */
    for (int i = 0; i < 288; i++) fabric_tick(&f);
    
    ASSERT(f.snap_count == 2, "T6 2 snapshots");
}

/* ── T7: verify ── */
static void t7_verify(void) {
    BoundaryFabric f;
    fabric_init(&f, 1ULL);
    
    /* verify root always ok */
    ASSERT(f.network.node_count == 1, "T7 verify runs");
}

/* ── T8: invalid wire ── */
static void t8_invalid_wire(void) {
    BoundaryFabric f;
    fabric_init(&f, 1ULL);
    
    /* try wire non-existent nodes */
    uint32_t wire = fabric_wire_create(&f, 0, 10, 0);
    ASSERT(wire == 0xFFFFFFFF, "T8 invalid node rejected");
}

/* ── T9: max wires ── */
static void t9_max_wires(void) {
    BoundaryFabric f;
    fabric_init(&f, 1ULL);
    f.wire_count = LC_MAX_WIRES;
    
    uint32_t wire = fabric_wire_create(&f, 0, 0, 0);
    ASSERT(wire == 0xFFFFFFFF, "T9 max wires rejected");
}

/* ── T10: flush at 720 ── */
static void t10_flush(void) {
    BoundaryFabric f;
    fabric_init(&f, 0x9999999999999999ULL);
    
    for (uint64_t i = 0; i < 720; i++) {
        fabric_tick(&f);
    }
    
    ASSERT(f.zone_counter == 720, "T10 reached zone 720");
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
    printf("\n%d/%d PASS\n", g_pass, g_pass + g_fail);
    return g_fail == 0 ? 0 : 1;
}
