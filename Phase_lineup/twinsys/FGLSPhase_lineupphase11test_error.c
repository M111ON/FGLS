/*
 * test_error.c — Phase 10 geo_error.h tests
 * Build: gcc -O2 -o test_error test_error.c && ./test_error
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "geo_error.h"

static int _tc = 0, _fail = 0;
#define CHECK(cond, name) do { \
    _tc++; \
    if (cond) { printf("PASS T%02d %s\n", _tc, name); } \
    else      { printf("FAIL T%02d %s  [line %d]\n", _tc, name, __LINE__); _fail++; } \
} while(0)

/* ── Input gate ── */
static void t_gate(void) {
    uint8_t buf[64] = {0};
    CHECK(geo_input_gate(buf, 64, 64) == GEO_ERR_OK,        "gate_ok");
    CHECK(geo_input_gate(NULL, 64, 64) == GEO_ERR_NULL_INPUT,"gate_null");
    CHECK(geo_input_gate(buf, 0, 64)   == GEO_ERR_SIZE_ZERO, "gate_zero");
    CHECK(geo_input_gate(buf, 65, 64)  == GEO_ERR_SIZE_ALIGN,"gate_align");
    CHECK(geo_input_gate(buf, 64, 0)   == GEO_ERR_OK,        "gate_no_align");
}

/* ── Checksum L1 ── */
static void t_checksum(void) {
    uint8_t buf[64];
    for (int i = 0; i < 64; i++) buf[i] = (uint8_t)(i + 1);
    uint32_t cs = geo_checksum(buf, 64);
    CHECK(cs != 0,                             "checksum_nonzero");
    CHECK(geo_checksum_verify(buf, 64, cs),    "checksum_verify_ok");
    buf[0] ^= 0xFF;
    CHECK(!geo_checksum_verify(buf, 64, cs),   "checksum_detect_flip");
    buf[0] ^= 0xFF; /* restore */
    uint8_t zeros[64] = {0};
    CHECK(geo_checksum(zeros, 64) == 0,        "checksum_zeros");
}

/* ── WHE-lite L0 ── */
static void t_whe(void) {
    GeoWhe w; geo_whe_init(&w);
    /* clean run */
    for (uint32_t i = 0; i < 720; i++)
        geo_whe_step(&w, GEO_WHE_PHI ^ i, GEO_WHE_PHI ^ i, i);
    CHECK(geo_whe_clean(&w),                   "whe_clean_run");
    CHECK(w.violations == 0,                   "whe_zero_violations");

    /* deviation */
    GeoWhe w2; geo_whe_init(&w2);
    geo_whe_step(&w2, 0xAAAA, 0xBBBB, 0);
    CHECK(!geo_whe_clean(&w2),                 "whe_detect_deviation");
    CHECK(w2.violations == 1,                  "whe_violation_count");

    /* fingerprint differs on deviation */
    GeoWhe wa; geo_whe_init(&wa);
    GeoWhe wb; geo_whe_init(&wb);
    geo_whe_step(&wa, 0x1234, 0x1234, 0);
    geo_whe_step(&wb, 0x1234, 0x9999, 0);
    CHECK(geo_whe_final(&wa) != geo_whe_final(&wb), "whe_fp_differs");

    /* replay deterministic */
    GeoWhe wc; geo_whe_init(&wc);
    GeoWhe wd; geo_whe_init(&wd);
    for (uint32_t i = 0; i < 60; i++) {
        uint64_t v = (uint64_t)i * 0x1111;
        geo_whe_step(&wc, v, v, i);
        geo_whe_step(&wd, v, v, i);
    }
    CHECK(geo_whe_final(&wc) == geo_whe_final(&wd), "whe_replay_deterministic");
}

/* ── Seg check ── */
static void t_seg(void) {
    CHECK(geo_seg_check(0, 10)  == GEO_ERR_OK,          "seg_ok");
    CHECK(geo_seg_check(9, 10)  == GEO_ERR_OK,          "seg_last_ok");
    CHECK(geo_seg_check(10, 10) == GEO_ERR_SEG_OVERFLOW,"seg_overflow");
    CHECK(geo_seg_check(0, 0)   == GEO_ERR_SEG_OVERFLOW,"seg_zero_total");
}

/* ── err_str ── */
static void t_str(void) {
    CHECK(geo_err_str(GEO_ERR_OK)[0] == 'O',            "str_ok");
    CHECK(geo_err_str(GEO_ERR_CHECKSUM)[0] == 'C',      "str_checksum");
    CHECK(geo_err_str(99)[0] == 'U',                    "str_unknown");
}

int main(void) {
    t_gate();
    t_checksum();
    t_whe();
    t_seg();
    t_str();
    printf("\n%d/%d PASS\n", _tc - _fail, _tc);
    return _fail ? 1 : 0;
}
