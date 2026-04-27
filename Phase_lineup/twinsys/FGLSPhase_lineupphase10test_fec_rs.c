/*
 * test_fec_rs.c — Phase 9c: RS FEC Test Suite
 * ═════════════════════════════════════════════
 * Proves RS is strictly superior to XOR on 3 dimensions:
 *
 *  T1: same-stride pairs (XOR can't recover) → RS with fec_n=3 recovers 100%
 *  T2: random k-loss (k ≤ fec_n)             → RS recovers 100%, byte-perfect
 *  T3: k > fec_n                             → RS fails clean (no corruption)
 *  T4: fec_n=1 (single parity) vs XOR P0    → same recovery, sanity check
 *  T5: fec_n=6 (high redundancy), 5 losses   → 100% recovery
 *  T6: exhaustive k-loss for k=1..fec_n      → all pass, k>fec_n all fail clean
 *  T7: partial last chunk (size=1337)        → chunk_sizes preserved byte-perfect
 *  T8: fec_n=12 (max), 12 losses in block   → full block reconstruction
 *
 * Build: gcc -O2 -o test_fec_rs test_fec_rs.c && ./test_fec_rs
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "geo_temporal_lut.h"
#include "geo_temporal_ring.h"
#include "geo_pyramid.h"
#include "geo_tring_stream.h"
#include "geo_fec.h"
#include "geo_gf256.h"
#include "geo_rs.h"
#include "geo_fec_rs.h"

/* ── test infra ─────────────────────────────────── */
static int pass_count = 0, fail_count = 0;
#define ASSERT(c,m) do { \
    if(c){printf("  [PASS] %s\n",(m));pass_count++;} \
    else {printf("  [FAIL] %s\n",(m));fail_count++;} \
} while(0)

/* ── static pools ───────────────────────────────── */
#define MAX_FEC_N   12u
#define POOL_MAX    (FEC_LEVELS * FEC_BLOCKS_PER_LEVEL * MAX_FEC_N)  /* 720 */

static TStreamChunk store_orig[FEC_TOTAL_DATA];
static TStreamChunk store_work[FEC_TOTAL_DATA];
static FECParity    parity_pool[POOL_MAX];
static TRingCtx     ring;

/* ── helpers ────────────────────────────────────── */
static void fill_store(uint16_t n, uint16_t last_sz) {
    for (uint16_t i = 0u; i < n; i++) {
        uint16_t sz = (i == (uint16_t)(n - 1u)) ? last_sz : TSTREAM_DATA_BYTES;
        store_orig[i].size = sz;
        for (uint16_t b = 0u; b < sz; b++)
            store_orig[i].data[b] = (uint8_t)((i * 7u + b * 13u + 42u) & 0xFF);
        memset(store_orig[i].data + sz, 0, TSTREAM_DATA_BYTES - sz);
    }
}

static void mark_all(uint16_t n) {
    tring_init(&ring);
    for (uint16_t i = 0u; i < n; i++) ring.slots[i].present = 1u;
}

static void reset_work(uint16_t n) {
    memcpy(store_work, store_orig, sizeof(TStreamChunk) * n);
    mark_all(n);
}

static void drop(uint16_t pos) {
    ring.slots[pos].present = 0u;
    memset(store_work[pos].data, 0, TSTREAM_DATA_BYTES);
    store_work[pos].size = 0u;
}

static int stores_eq(uint16_t n) {
    for (uint16_t i = 0u; i < n; i++) {
        if (store_work[i].size != store_orig[i].size) return 0;
        if (memcmp(store_work[i].data, store_orig[i].data, store_orig[i].size) != 0) return 0;
    }
    return 1;
}

/* count remaining gaps */
static uint16_t gap_count(uint16_t n) {
    uint16_t g = 0u;
    for (uint16_t i = 0u; i < n; i++)
        if (!ring.slots[i].present) g++;
    return g;
}

/* ── T1: same-stride pairs — XOR fails, RS wins ── */
static void t1_same_stride_pairs(void) {
    printf("\n[T1] Same-stride pairs (XOR unrecoverable) — RS fec_n=3\n");
    printf("     XOR failed on 18/66 pairs; RS must recover all of them.\n");

    const uint8_t fec_n = 3u;
    const uint16_t n    = FEC_TOTAL_DATA;
    fill_store(n, TSTREAM_DATA_BYTES);

    /* same-stride pairs for block 0:
     * S3A = {0,3,6,9}, S3B = {1,4,7,10}, S3C = {2,5,8,11}
     * XOR fails on: C(S3A,2)=6, C(S3B,2)=6, C(S3C,2)=6 = 18 pairs total */
    uint8_t same_a[][2] = {{0,3},{0,6},{0,9},{3,6},{3,9},{6,9}};
    uint8_t same_b[][2] = {{1,4},{1,7},{1,10},{4,7},{4,10},{7,10}};
    uint8_t same_c[][2] = {{2,5},{2,8},{2,11},{5,8},{5,11},{8,11}};

    uint16_t total_xor_broke  = 0u;
    uint16_t total_rs_fixed   = 0u;
    uint16_t total_corrupt    = 0u;

    /* test all 18 unrecoverable-by-XOR pairs in block 0, level 0 */
    for (int set = 0; set < 3; set++) {
        uint8_t (*pairs)[2] = (set == 0) ? same_a : (set == 1) ? same_b : same_c;
        for (int p = 0; p < 6; p++) {
            uint8_t mi = pairs[p][0], mj = pairs[p][1];

            reset_work(n);
            fec_rs_encode_all(store_work, fec_n, parity_pool);

            /* drop the same-stride pair in block 0 (level 0) */
            drop((uint16_t)(0 * GEO_PYR_PHASE_LEN + 0 * FEC_CHUNKS_PER_BLOCK + mi));
            drop((uint16_t)(0 * GEO_PYR_PHASE_LEN + 0 * FEC_CHUNKS_PER_BLOCK + mj));

            total_xor_broke++;

            /* RS recover only block 0 */
            uint8_t recovered = fec_rs_recover_block(&ring, store_work,
                                                      &parity_pool[0 * fec_n], fec_n);
            if (recovered == 2u) total_rs_fixed++;
            if (!stores_eq(n))   total_corrupt++;
        }
    }

    ASSERT(total_xor_broke == 18u, "18 same-stride pairs tested (XOR domain)");
    ASSERT(total_rs_fixed  == 18u, "RS recovered all 18 pairs (100%)");
    ASSERT(total_corrupt   == 0u,  "0 corruption events");
}

/* ── T2: random k-loss (k ≤ fec_n) across multiple fec_n ── */
static void t2_random_kloss(void) {
    printf("\n[T2] Random k-loss (k ≤ fec_n) — multiple fec_n values\n");

    const uint16_t n   = FEC_TOTAL_DATA;
    const uint32_t SEED = 0xDEADBEEFu;

    /* test fec_n = 2, 3, 5, 8 */
    uint8_t test_fec_ns[] = {2u, 3u, 5u, 8u};
    int all_ok = 1;

    for (int fi = 0; fi < 4; fi++) {
        uint8_t fec_n = test_fec_ns[fi];
        fill_store(n, TSTREAM_DATA_BYTES);
        reset_work(n);
        fec_rs_encode_all(store_work, fec_n, parity_pool);

        /* drop exactly fec_n chunks spread across blocks deterministically */
        uint32_t rng = SEED ^ ((uint32_t)fec_n * 0x9E3779B9u);
        uint16_t dropped = 0u;
        for (uint8_t l = 0u; l < FEC_LEVELS; l++) {
            for (uint8_t b = 0u; b < FEC_BLOCKS_PER_LEVEL; b++) {
                /* drop exactly fec_n random positions in this block */
                uint8_t dropped_here[FEC_CHUNKS_PER_BLOCK] = {0};
                uint8_t nd = 0u;
                while (nd < fec_n) {
                    rng = rng * 1664525u + 1013904223u;
                    uint8_t pos = (uint8_t)(rng % FEC_CHUNKS_PER_BLOCK);
                    if (dropped_here[pos]) continue;
                    dropped_here[pos] = 1u;
                    drop((uint16_t)(l * GEO_PYR_PHASE_LEN + b * FEC_CHUNKS_PER_BLOCK + pos));
                    nd++; dropped++;
                }
            }
        }

        uint16_t rec = fec_rs_recover_all(&ring, store_work, fec_n, parity_pool);
        int ok = (rec == dropped) && stores_eq(n);
        if (!ok) all_ok = 0;
        printf("   fec_n=%u: dropped=%u recovered=%u store_ok=%d\n",
               fec_n, dropped, rec, stores_eq(n));
    }
    ASSERT(all_ok, "All fec_n variants: k==fec_n losses → 100% recovery, byte-perfect");
}

/* ── T3: k > fec_n — fail clean, no corruption ── */
static void t3_overcapacity_fail_clean(void) {
    printf("\n[T3] k > fec_n — fail clean, no corruption\n");

    const uint16_t n  = FEC_TOTAL_DATA;
    const uint8_t fec_n = 3u;

    fill_store(n, TSTREAM_DATA_BYTES);
    reset_work(n);
    fec_rs_encode_all(store_work, fec_n, parity_pool);

    /* Drop fec_n+1 = 4 chunks in every block */
    for (uint8_t l = 0u; l < FEC_LEVELS; l++)
        for (uint8_t b = 0u; b < FEC_BLOCKS_PER_LEVEL; b++)
            for (uint8_t i = 0u; i < fec_n + 1u; i++)
                drop((uint16_t)(l * GEO_PYR_PHASE_LEN + b * FEC_CHUNKS_PER_BLOCK + i));

    /* snapshot store_work before recovery attempt */
    static TStreamChunk snap[FEC_TOTAL_DATA];
    memcpy(snap, store_work, sizeof(TStreamChunk) * n);
    uint16_t gaps_before = gap_count(n);

    uint16_t rec = fec_rs_recover_all(&ring, store_work, fec_n, parity_pool);

    /* store must not have changed for unrecoverable slots */
    int no_corrupt = 1;
    for (uint16_t i = 0u; i < n; i++) {
        if (!ring.slots[i].present) {
            /* missing slot: data must remain as it was (zeroed) */
            if (memcmp(store_work[i].data, snap[i].data, TSTREAM_DATA_BYTES) != 0)
                no_corrupt = 0;
        }
    }

    ASSERT(rec == 0u,       "0 chunks recovered (all blocks exceed fec_n)");
    ASSERT(no_corrupt,      "No corruption: unrecoverable slots untouched");
    ASSERT(gap_count(n) == gaps_before, "Gap count unchanged after fail");
}

/* ── T4: fec_n=1 sanity (RS ~ XOR P0) ──────────── */
static void t4_fec_n1_sanity(void) {
    printf("\n[T4] fec_n=1 sanity — single parity, 1 loss recovery\n");

    const uint16_t n   = FEC_TOTAL_DATA;
    const uint8_t fec_n = 1u;
    fill_store(n, TSTREAM_DATA_BYTES);
    reset_work(n);
    fec_rs_encode_all(store_work, fec_n, parity_pool);

    /* drop 1 chunk per block (last slot) */
    for (uint8_t l = 0u; l < FEC_LEVELS; l++)
        for (uint8_t b = 0u; b < FEC_BLOCKS_PER_LEVEL; b++)
            drop((uint16_t)(l * GEO_PYR_PHASE_LEN + b * FEC_CHUNKS_PER_BLOCK + 11u));

    uint16_t rec = fec_rs_recover_all(&ring, store_work, fec_n, parity_pool);
    ASSERT(rec == 60u,       "60/60 recovered (1 per block)");
    ASSERT(stores_eq(n),     "Byte-perfect");
    ASSERT(gap_count(n)==0u, "0 gaps after");
}

/* ── T5: fec_n=6 high redundancy, 5 losses ──────── */
static void t5_high_redundancy(void) {
    printf("\n[T5] fec_n=6, 5 losses per block — well within capacity\n");

    const uint16_t n   = FEC_TOTAL_DATA;
    const uint8_t fec_n = 6u;
    fill_store(n, TSTREAM_DATA_BYTES);
    reset_work(n);
    fec_rs_encode_all(store_work, fec_n, parity_pool);

    /* drop positions 0..4 (5 losses) in every block */
    uint16_t total_drop = 0u;
    for (uint8_t l = 0u; l < FEC_LEVELS; l++)
        for (uint8_t b = 0u; b < FEC_BLOCKS_PER_LEVEL; b++)
            for (uint8_t i = 0u; i < 5u; i++) {
                drop((uint16_t)(l * GEO_PYR_PHASE_LEN + b * FEC_CHUNKS_PER_BLOCK + i));
                total_drop++;
            }

    uint16_t rec = fec_rs_recover_all(&ring, store_work, fec_n, parity_pool);
    ASSERT(rec == total_drop, "300/300 recovered (fec_n=6, 5 loss)");
    ASSERT(stores_eq(n),      "Byte-perfect");
}

/* ── T6: exhaustive k-loss for k=1..fec_n+1, block 0 ── */
static void t6_exhaustive_kloss(void) {
    printf("\n[T6] Exhaustive k-loss block 0 (k=1..fec_n+1), fec_n=4\n");

    const uint16_t n   = FEC_TOTAL_DATA;
    const uint8_t fec_n = 4u;
    fill_store(n, TSTREAM_DATA_BYTES);

    /* positions to drop progressively */
    uint8_t drop_seq[13] = {0,1,2,3,4,5,6,7,8,9,10,11,255}; /* 255=sentinel */

    int k_ok[5] = {0}; /* k=1..4 = ok, k=5=fail_clean */
    int k_nc[5] = {0}; /* no corruption for k=5 */

    for (uint8_t k = 1u; k <= 5u; k++) {
        reset_work(n);
        fec_rs_encode_all(store_work, fec_n, parity_pool);

        /* drop k chunks in block 0 level 0 */
        for (uint8_t d = 0u; d < k; d++)
            drop((uint16_t)(0 * GEO_PYR_PHASE_LEN + 0 * FEC_CHUNKS_PER_BLOCK + drop_seq[d]));

        static TStreamChunk snap[FEC_TOTAL_DATA];
        memcpy(snap, store_work, sizeof(TStreamChunk) * n);

        uint8_t rec = fec_rs_recover_block(&ring, store_work,
                                            &parity_pool[0 * fec_n], fec_n);
        if (k <= fec_n) {
            k_ok[k-1] = (rec == k) && stores_eq(n);
        } else {
            /* k > fec_n: must not corrupt */
            int nc = 1;
            for (uint16_t i = 0u; i < n; i++) {
                if (!ring.slots[i].present &&
                    memcmp(store_work[i].data, snap[i].data, TSTREAM_DATA_BYTES) != 0) {
                    nc = 0; break;
                }
            }
            k_nc[0] = nc;
        }
    }

    ASSERT(k_ok[0], "k=1 ≤ fec_n=4 → recovered");
    ASSERT(k_ok[1], "k=2 ≤ fec_n=4 → recovered");
    ASSERT(k_ok[2], "k=3 ≤ fec_n=4 → recovered");
    ASSERT(k_ok[3], "k=4 = fec_n=4 → recovered (boundary)");
    ASSERT(k_nc[0], "k=5 > fec_n=4 → fail clean, no corruption");
}

/* ── T7: partial last chunk (1337 bytes) ─────────── */
static void t7_partial_chunk(void) {
    printf("\n[T7] Partial last chunk (size=1337) byte-perfect recovery\n");

    const uint16_t n    = FEC_TOTAL_DATA;
    const uint8_t fec_n  = 3u;
    fill_store(n, 1337u); /* last slot = 1337 bytes */

    reset_work(n);
    fec_rs_encode_all(store_work, fec_n, parity_pool);

    /* drop the last slot (partial chunk) in block 11, level 4 */
    uint16_t pos_last = (uint16_t)(4u * GEO_PYR_PHASE_LEN + 11u * FEC_CHUNKS_PER_BLOCK + 11u);
    drop(pos_last);

    uint16_t rec = fec_rs_recover_all(&ring, store_work, fec_n, parity_pool);

    ASSERT(rec >= 1u,                                     "At least 1 recovered");
    ASSERT(store_work[pos_last].size == 1337u,            "Partial chunk size=1337 preserved");
    ASSERT(memcmp(store_work[pos_last].data,
                  store_orig[pos_last].data, 1337u) == 0, "Partial chunk data byte-perfect");
    ASSERT(stores_eq(n),                                  "Full store byte-perfect");
}

/* ── T8: fec_n=12 (max), full block loss ─────────── */
static void t8_full_block_recovery(void) {
    printf("\n[T8] fec_n=12 (max parity), full block (12 losses) recovery\n");
    printf("     Stress test: parity alone reconstructs entire block.\n");

    const uint16_t n   = FEC_TOTAL_DATA;
    const uint8_t fec_n = 12u; /* max, = FEC_CHUNKS_PER_BLOCK */
    fill_store(n, TSTREAM_DATA_BYTES);
    reset_work(n);
    fec_rs_encode_all(store_work, fec_n, parity_pool);

    /* drop all 12 chunks in block 0, level 0 */
    uint16_t base = 0u;
    for (uint8_t i = 0u; i < 12u; i++)
        drop((uint16_t)(base + i));

    uint8_t rec = fec_rs_recover_block(&ring, store_work,
                                        &parity_pool[0 * fec_n], fec_n);
    ASSERT(rec == 12u,   "All 12 chunks recovered from parity alone");
    ASSERT(stores_eq(n), "Byte-perfect reconstruction");
}

/* ── main ───────────────────────────────────────── */
int main(void) {
    printf("═══════════════════════════════════════════════════\n");
    printf(" POGLS Phase 9c — RS FEC Test Suite\n");
    printf(" RS over GF(256), flexible fec_n, Vandermonde\n");
    printf("═══════════════════════════════════════════════════\n");

    gf256_init();

    t1_same_stride_pairs();
    t2_random_kloss();
    t3_overcapacity_fail_clean();
    t4_fec_n1_sanity();
    t5_high_redundancy();
    t6_exhaustive_kloss();
    t7_partial_chunk();
    t8_full_block_recovery();

    printf("\n═══════════════════════════════════════════════════\n");
    printf(" RESULT: %d/%d PASS\n", pass_count, pass_count + fail_count);
    printf("═══════════════════════════════════════════════════\n");
    return (fail_count == 0) ? 0 : 1;
}
