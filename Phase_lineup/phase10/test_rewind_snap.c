/*
 * test_rewind_snap.c — Snapshot (version control) tests for geo_rewind.h
 * Build: gcc -O2 -o test_rewind_snap test_rewind_snap.c && ./test_rewind_snap
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "geo_rewind.h"

static int _tc = 0, _fail = 0;
#define CHECK(cond, name) do { \
    _tc++; \
    if (cond) { printf("PASS T%02d %s\n", _tc, name); } \
    else      { printf("FAIL T%02d %s  [line %d]\n", _tc, name, __LINE__); _fail++; } \
} while(0)

static TStreamChunk make_chunk(uint8_t fill) {
    TStreamChunk c; memset(&c, 0, sizeof(c));
    memset(c.data, fill, TSTREAM_DATA_BYTES);
    c.size = TSTREAM_DATA_BYTES;
    return c;
}

/* ── T01: take snapshot on empty buffer ── */
static void t_snap_empty(void) {
    static RewindBuffer rb; rewind_init(&rb);
    bool ok = rewind_snapshot_take(&rb, 0);
    CHECK(ok, "snap_take_on_empty");
    RewindStats st; rewind_stats(&rb, &st);
    CHECK(st.snapshots == 1u, "snap_count_1");
}

/* ── T02: drop snapshot releases it ── */
static void t_snap_drop(void) {
    static RewindBuffer rb; rewind_init(&rb);
    rewind_snapshot_take(&rb, 0);
    bool ok = rewind_snapshot_drop(&rb, 0);
    CHECK(ok, "snap_drop_ok");
    RewindStats st; rewind_stats(&rb, &st);
    CHECK(st.snapshots == 0u, "snap_count_0_after_drop");
}

/* ── T03: slots written before snap are pinned (not evicted by new stores) ── */
static void t_snap_pins_slots(void) {
    static RewindBuffer rb; rewind_init(&rb);
    TStreamChunk c = make_chunk(0xAA);
    rewind_store(&rb, 1u, &c);
    rewind_snapshot_take(&rb, 0);

    /* overflow buffer to force eviction of unpinned slots */
    TStreamChunk dummy = make_chunk(0xBB);
    for (uint32_t i = 2u; i < REWIND_SLOTS + 100u; i++)
        rewind_store(&rb, i, &dummy);

    /* enc=1 must still be findable — pinned */
    const TStreamChunk *r = rewind_find(&rb, 1u);
    CHECK(r != NULL && r->data[0] == 0xAA, "pinned_slot_survives_eviction");

    rewind_snapshot_drop(&rb, 0);
}

/* ── T04: slots written AFTER snap are evictable ── */
static void t_post_snap_evictable(void) {
    static RewindBuffer rb; rewind_init(&rb);
    rewind_snapshot_take(&rb, 0); /* snap empty buffer */

    TStreamChunk c = make_chunk(0xCC);
    rewind_store(&rb, 999u, &c);

    /* overflow to evict post-snap slot */
    TStreamChunk dummy = make_chunk(0xDD);
    for (uint32_t i = 1000u; i < 1000u + REWIND_SLOTS; i++)
        rewind_store(&rb, i, &dummy);

    /* enc=999 should be evicted (not pinned) */
    CHECK(rewind_find(&rb, 999u) == NULL, "post_snap_slot_evictable");
    rewind_snapshot_drop(&rb, 0);
}

/* ── T05: restore reverts to snapshot state ── */
static void t_snap_restore(void) {
    static RewindBuffer rb; rewind_init(&rb);
    TStreamChunk good = make_chunk(0x11);
    rewind_store(&rb, 10u, &good);
    rewind_store(&rb, 20u, &good);
    rewind_snapshot_take(&rb, 0); /* checkpoint: {enc=10, enc=20} */

    /* add more data after snapshot */
    TStreamChunk bad = make_chunk(0x22);
    rewind_store(&rb, 30u, &bad);
    rewind_store(&rb, 40u, &bad);

    /* restore to checkpoint */
    bool ok = rewind_snapshot_restore(&rb, 0);
    CHECK(ok, "snap_restore_ok");

    /* slots 10,20 should be present; 30,40 cleared */
    CHECK(rewind_has(&rb, 10u), "restore_has_enc10");
    CHECK(rewind_has(&rb, 20u), "restore_has_enc20");
    CHECK(!rewind_has(&rb, 30u), "restore_no_enc30");
    CHECK(!rewind_has(&rb, 40u), "restore_no_enc40");

    rewind_snapshot_drop(&rb, 0);
}

/* ── T06: restore can be called multiple times ── */
static void t_snap_restore_idempotent(void) {
    static RewindBuffer rb; rewind_init(&rb);
    TStreamChunk c = make_chunk(0x55);
    rewind_store(&rb, 5u, &c);
    rewind_snapshot_take(&rb, 0);

    rewind_store(&rb, 6u, &c);
    rewind_snapshot_restore(&rb, 0);
    rewind_store(&rb, 7u, &c);
    rewind_snapshot_restore(&rb, 0); /* restore again */

    CHECK(rewind_has(&rb, 5u), "restore_2x_enc5_intact");
    CHECK(!rewind_has(&rb, 6u), "restore_2x_enc6_cleared");
    CHECK(!rewind_has(&rb, 7u), "restore_2x_enc7_cleared");
    rewind_snapshot_drop(&rb, 0);
}

/* ── T07: 4 simultaneous snapshots ── */
static void t_snap_multi(void) {
    static RewindBuffer rb; rewind_init(&rb);
    TStreamChunk c = make_chunk(0x01);
    rewind_store(&rb, 1u, &c);
    rewind_snapshot_take(&rb, 0);
    rewind_store(&rb, 2u, &c);
    rewind_snapshot_take(&rb, 1);
    rewind_store(&rb, 3u, &c);
    rewind_snapshot_take(&rb, 2);
    rewind_store(&rb, 4u, &c);
    rewind_snapshot_take(&rb, 3);

    RewindStats st; rewind_stats(&rb, &st);
    CHECK(st.snapshots == 4u, "multi_4_snapshots");

    /* 5th snapshot should fail */
    CHECK(!rewind_snapshot_take(&rb, 0), "5th_snap_rejected");

    rewind_snapshot_drop(&rb, 0);
    rewind_snapshot_drop(&rb, 1);
    rewind_snapshot_drop(&rb, 2);
    rewind_snapshot_drop(&rb, 3);
}

/* ── T08: bad id rejected ── */
static void t_snap_bad_id(void) {
    static RewindBuffer rb; rewind_init(&rb);
    CHECK(!rewind_snapshot_take(&rb, REWIND_MAX_SNAPSHOTS), "take_bad_id");
    CHECK(!rewind_snapshot_restore(&rb, REWIND_MAX_SNAPSHOTS), "restore_bad_id");
    CHECK(!rewind_snapshot_drop(&rb, REWIND_MAX_SNAPSHOTS), "drop_bad_id");
}

/* ── T09: restore on inactive snap returns false ── */
static void t_snap_restore_inactive(void) {
    static RewindBuffer rb; rewind_init(&rb);
    CHECK(!rewind_snapshot_restore(&rb, 0), "restore_inactive_snap");
    CHECK(!rewind_snapshot_drop(&rb, 0), "drop_inactive_snap");
}

/* ── T10: data content intact after restore ── */
static void t_snap_data_integrity(void) {
    static RewindBuffer rb; rewind_init(&rb);
    TStreamChunk c; memset(&c, 0, sizeof(c)); c.size = TSTREAM_DATA_BYTES;
    for (int i = 0; i < (int)TSTREAM_DATA_BYTES; i++)
        c.data[i] = (uint8_t)(i ^ 0xA5);
    rewind_store(&rb, 100u, &c);
    rewind_snapshot_take(&rb, 0);

    /* add noise then restore */
    TStreamChunk noise = make_chunk(0xFF);
    for (uint32_t i = 101u; i < 200u; i++)
        rewind_store(&rb, i, &noise);
    rewind_snapshot_restore(&rb, 0);

    const TStreamChunk *r = rewind_find(&rb, 100u);
    int ok = (r != NULL);
    if (ok) for (int i = 0; i < (int)TSTREAM_DATA_BYTES; i++)
        ok &= (r->data[i] == (uint8_t)(i ^ 0xA5));
    CHECK(ok, "snap_data_integrity_after_restore");
    rewind_snapshot_drop(&rb, 0);
}

/* ── T11: pinned stats correct ── */
static void t_snap_pinned_stats(void) {
    static RewindBuffer rb; rewind_init(&rb);
    TStreamChunk c = make_chunk(0x01);
    for (uint32_t i = 1u; i <= 10u; i++)
        rewind_store(&rb, i, &c);
    rewind_snapshot_take(&rb, 0);

    RewindStats st; rewind_stats(&rb, &st);
    CHECK(st.pinned == 10u, "pinned_stats_10");
    CHECK(st.snapshots == 1u, "snapshot_count_1");

    rewind_snapshot_drop(&rb, 0);
    rewind_stats(&rb, &st);
    CHECK(st.pinned == 0u, "pinned_0_after_drop");
}

/* ── T12: use case — last known good recovery ── */
static void t_snap_last_known_good(void) {
    static RewindBuffer rb; rewind_init(&rb);

    /* simulate: receive a good window of data */
    TStreamChunk good = make_chunk(0xAA);
    for (uint32_t i = 0u; i < 60u; i++)
        rewind_store(&rb, GEO_WALK[i], &good);

    /* mark as "last known good" */
    rewind_snapshot_take(&rb, 0);

    /* simulate: receive bad/corrupt window */
    TStreamChunk bad = make_chunk(0x00);
    for (uint32_t i = 60u; i < 120u; i++)
        rewind_store(&rb, GEO_WALK[i], &bad);

    /* loss exceeds RS capacity — roll back */
    rewind_snapshot_restore(&rb, 0);

    /* verify good data still accessible */
    int ok = 1;
    for (uint32_t i = 0u; i < 60u; i++) {
        const TStreamChunk *r = rewind_find(&rb, GEO_WALK[i]);
        ok &= (r != NULL && r->data[0] == 0xAA);
    }
    /* bad window cleared */
    for (uint32_t i = 60u; i < 120u; i++)
        ok &= !rewind_has(&rb, GEO_WALK[i]);

    CHECK(ok, "last_known_good_rollback");
    rewind_snapshot_drop(&rb, 0);
}

int main(void) {
    t_snap_empty();
    t_snap_drop();
    t_snap_pins_slots();
    t_post_snap_evictable();
    t_snap_restore();
    t_snap_restore_idempotent();
    t_snap_multi();
    t_snap_bad_id();
    t_snap_restore_inactive();
    t_snap_data_integrity();
    t_snap_pinned_stats();
    t_snap_last_known_good();

    printf("\n%d/%d PASS\n", _tc - _fail, _tc);
    return _fail ? 1 : 0;
}
