#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "geo_rewind.h"

/* ── harness ── */
static int pass_count = 0, fail_count = 0;
#define CHECK(cond, name) do { \
    if (cond) { printf("PASS T%02d %s\n", ++pass_count, name); } \
    else       { printf("FAIL T%02d %s  [line %d]\n", ++pass_count, name, __LINE__); fail_count++; } \
} while(0)

static TStreamChunk make_chunk(uint8_t fill, uint16_t size) {
    TStreamChunk c;
    memset(&c, 0, sizeof(c));
    memset(c.data, fill, sizeof(c.data));
    c.size = size;
    return c;
}

/* ── tests ── */

/* T01-T03: basic store/find */
static void t_basic(void) {
    RewindBuffer rb; rewind_init(&rb);
    TStreamChunk c1 = make_chunk(0xAA, 100);
    TStreamChunk c2 = make_chunk(0xBB, 200);

    rewind_store(&rb, 1, &c1);
    rewind_store(&rb, 2, &c2);

    const TStreamChunk *r1 = rewind_find(&rb, 1);
    CHECK(r1 && r1->data[0] == 0xAA, "basic_find_enc1");

    const TStreamChunk *r2 = rewind_find(&rb, 2);
    CHECK(r2 && r2->data[0] == 0xBB, "basic_find_enc2");

    CHECK(rewind_find(&rb, 99) == NULL, "find_missing_returns_null");
}

/* T04: rewind_has */
static void t_has(void) {
    RewindBuffer rb; rewind_init(&rb);
    TStreamChunk c = make_chunk(1, 50);
    rewind_store(&rb, 42, &c);
    CHECK(rewind_has(&rb, 42),   "has_stored");
    CHECK(!rewind_has(&rb, 43),  "has_not_stored");
}

/* T05: stats after stores */
static void t_stats(void) {
    RewindBuffer rb; rewind_init(&rb);
    for (uint32_t i = 1; i <= 10; i++) {
        TStreamChunk c = make_chunk((uint8_t)i, 100);
        rewind_store(&rb, i, &c);
    }
    RewindStats st; rewind_stats(&rb, &st);
    CHECK(st.stored == 10 && st.occupied == 10, "stats_10_stored");
}

/* T06: init → all empty */
static void t_init_clean(void) {
    RewindBuffer rb; rewind_init(&rb);
    RewindStats st; rewind_stats(&rb, &st);
    CHECK(st.stored == 0 && st.occupied == 0, "init_clean");
}

/* T07: overwrite same enc — latest wins */
static void t_overwrite(void) {
    RewindBuffer rb; rewind_init(&rb);
    TStreamChunk c1 = make_chunk(0x11, 100);
    TStreamChunk c2 = make_chunk(0x22, 100);
    rewind_store(&rb, 10, &c1);
    rewind_store(&rb, 10, &c2);  /* same enc, different slot (circular) */
    /* fallback scan must find at least one, latest = 0x22 */
    const TStreamChunk *r = rewind_find(&rb, 10);
    CHECK(r != NULL, "overwrite_findable");
}

/* T08: circular fill — store REWIND_SLOTS+50 items, early ones evicted */
static void t_circular_eviction(void) {
    RewindBuffer rb; rewind_init(&rb);
    for (uint32_t i = 0; i < REWIND_SLOTS + 50; i++) {
        TStreamChunk c = make_chunk((uint8_t)(i & 0xFF), 100);
        rewind_store(&rb, i, &c);
    }
    /* earliest enc=0..49 should be evicted */
    CHECK(rewind_find(&rb, 0) == NULL, "evicted_enc0");
    /* recent ones must still be present */
    CHECK(rewind_find(&rb, REWIND_SLOTS + 49) != NULL, "recent_survives");
}

/* T09: store count monotone */
static void t_stored_count(void) {
    RewindBuffer rb; rewind_init(&rb);
    for (uint32_t i = 0; i < 100; i++) {
        TStreamChunk c = make_chunk(1, 100);
        rewind_store(&rb, i, &c);
    }
    RewindStats st; rewind_stats(&rb, &st);
    CHECK(st.stored == 100, "stored_count_100");
}

/* T10: data integrity — byte content preserved */
static void t_data_integrity(void) {
    RewindBuffer rb; rewind_init(&rb);
    TStreamChunk c; memset(&c, 0, sizeof(c)); c.size = 4096;
    for (int i = 0; i < (int)TSTREAM_DATA_BYTES; i++)
        c.data[i] = (uint8_t)(i ^ 0xA5);
    rewind_store(&rb, 777, &c);
    const TStreamChunk *r = rewind_find(&rb, 777);
    int ok = (r != NULL);
    if (ok) for (int i = 0; i < (int)TSTREAM_DATA_BYTES; i++)
        ok &= (r->data[i] == (uint8_t)(i ^ 0xA5));
    CHECK(ok, "data_integrity_4096B");
}

/* T11: size field preserved */
static void t_size_preserved(void) {
    RewindBuffer rb; rewind_init(&rb);
    TStreamChunk c = make_chunk(0, 1234);
    rewind_store(&rb, 5, &c);
    const TStreamChunk *r = rewind_find(&rb, 5);
    CHECK(r && r->size == 1234, "size_field_preserved");
}

/* T12: multiple enc from full walk range (0–719) */
static void t_walk_range(void) {
    RewindBuffer rb; rewind_init(&rb);
    for (uint32_t enc = 0; enc < TEMPORAL_WALK_LEN; enc++) {
        TStreamChunk c = make_chunk((uint8_t)(enc & 0xFF), 100);
        rewind_store(&rb, enc, &c);
    }
    uint32_t spot_encs[] = {0,1,100,500,719};
    int ok = 1;
    for (int si = 0; si < 5; si++) {
        uint32_t enc = spot_encs[si];
        const TStreamChunk *r = rewind_find(&rb, enc);
        ok &= (r != NULL && r->data[0] == (uint8_t)(enc & 0xFF));
    }
    CHECK(ok, "walk_range_720_spots");
}

/* T13: enc beyond 0x7FF (masked by tring_pos) — fallback finds it */
static void t_enc_high(void) {
    RewindBuffer rb; rewind_init(&rb);
    TStreamChunk c = make_chunk(0xFF, 100);
    rewind_store(&rb, 0x1000, &c);  /* enc > 0x7FF */
    CHECK(rewind_find(&rb, 0x1000) != NULL, "enc_high_fallback");
}

/* T14: occupied count matches actual valid slots after circular wrap */
static void t_occupied_after_wrap(void) {
    RewindBuffer rb; rewind_init(&rb);
    for (uint32_t i = 0; i < REWIND_SLOTS; i++) {
        TStreamChunk c = make_chunk(1, 100);
        rewind_store(&rb, i, &c);
    }
    RewindStats st; rewind_stats(&rb, &st);
    CHECK(st.occupied == REWIND_SLOTS, "occupied_full");
}

/* T15: head wraps cleanly — no off-by-one */
static void t_head_wrap(void) {
    RewindBuffer rb; rewind_init(&rb);
    for (uint32_t i = 0; i < REWIND_SLOTS * 2; i++) {
        TStreamChunk c = make_chunk((uint8_t)(i&0xFF), 100);
        rewind_store(&rb, i, &c);
    }
    RewindStats st; rewind_stats(&rb, &st);
    CHECK(st.stored == REWIND_SLOTS * 2, "head_wrap_stored");
}

/* T16: two encs that collide at same tring_pos — fallback finds both (or latest) */
static void t_tring_pos_collision(void) {
    RewindBuffer rb; rewind_init(&rb);
    /* enc=0 and enc=0x800 both map to tring_pos(0) since (enc&0x7FF)==0 */
    TStreamChunk c0 = make_chunk(0x01, 100);
    TStreamChunk c1 = make_chunk(0x02, 100);
    rewind_store(&rb, 0,      &c0);
    rewind_store(&rb, 0x800,  &c1);
    /* both should be findable (in different physical slots) */
    CHECK(rewind_find(&rb, 0)     != NULL, "collision_enc0_found");
    CHECK(rewind_find(&rb, 0x800) != NULL, "collision_enc800_found");
}

/* T17: find on empty buffer never crashes */
static void t_find_empty(void) {
    RewindBuffer rb; rewind_init(&rb);
    CHECK(rewind_find(&rb, 12345) == NULL, "find_on_empty");
}

/* T18: stored counter reflects total stores including overwrites */
static void t_stored_counter_monotone(void) {
    RewindBuffer rb; rewind_init(&rb);
    for (int i = 0; i < 200; i++) {
        TStreamChunk c = make_chunk(1, 100);
        rewind_store(&rb, (uint32_t)(i % 50), &c);  /* same 50 encs, reused */
    }
    RewindStats st; rewind_stats(&rb, &st);
    CHECK(st.stored == 200, "stored_counter_200");
}

int main(void) {
    t_basic();
    t_has();
    t_stats();
    t_init_clean();
    t_overwrite();
    t_circular_eviction();
    t_stored_count();
    t_data_integrity();
    t_size_preserved();
    t_walk_range();
    t_enc_high();
    t_occupied_after_wrap();
    t_head_wrap();
    t_tring_pos_collision();
    t_find_empty();
    t_stored_counter_monotone();

    printf("\n%d/%d PASS\n", pass_count - fail_count, pass_count);
    return fail_count ? 1 : 0;
}
