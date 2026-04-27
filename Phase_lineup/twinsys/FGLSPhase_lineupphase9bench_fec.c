/*
 * bench_fec.c — Phase 9c FEC Throughput Benchmark
 * ═════════════════════════════════════════════════
 * Measures encode + recover throughput for:
 *   - XOR FEC (Phase 9b baseline)
 *   - RS  FEC (Phase 9c, flexible fec_n)
 *
 * Metrics:
 *   - Encode MB/s  (data bytes / encode time)
 *   - Recover MB/s (recovered bytes / recover time)
 *   - Overhead: parity size as % of data
 *
 * Sweep: fec_n = 1, 2, 3, 4, 6, 8, 12
 * Iterations: BENCH_ITER runs per config (averaged)
 *
 * Build: gcc -O2 -o bench_fec bench_fec.c && ./bench_fec
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "geo_temporal_lut.h"
#include "geo_temporal_ring.h"
#include "geo_pyramid.h"
#include "geo_tring_stream.h"
#include "geo_fec.h"
#include "geo_gf256.h"
#include "geo_rs.h"
#include "geo_fec_rs.h"

/* ── config ─────────────────────────────────────── */
#define BENCH_ITER       8u        /* runs to average */
#define DATA_CHUNKS      FEC_TOTAL_DATA           /* 720 */
#define DATA_BYTES_TOTAL ((uint32_t)DATA_CHUNKS * TSTREAM_DATA_BYTES)  /* ~2.9 MB */

/* ── static pools (never on stack) ─────────────── */
static TStreamChunk store_orig[DATA_CHUNKS];
static TStreamChunk store_work[DATA_CHUNKS];
static FECParity    xor_parity[FEC_TOTAL_PARITY];              /* 180, fixed */
static FECParity    rs_parity [FEC_LEVELS * FEC_BLOCKS_PER_LEVEL * RS_MAX_N]; /* 720 max */
static TRingCtx     ring;

/* ── timer helpers ──────────────────────────────── */
static inline double now_sec(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* ── fill store with deterministic data ─────────── */
static void fill_store(void) {
    for (uint16_t i = 0u; i < DATA_CHUNKS; i++) {
        store_orig[i].size = TSTREAM_DATA_BYTES;
        for (uint16_t b = 0u; b < TSTREAM_DATA_BYTES; b++)
            store_orig[i].data[b] = (uint8_t)((i * 7u + b * 13u + 42u) & 0xFF);
    }
}

static void reset_work(void) {
    memcpy(store_work, store_orig, sizeof(TStreamChunk) * DATA_CHUNKS);
    tring_init(&ring);
    for (uint16_t i = 0u; i < DATA_CHUNKS; i++)
        ring.slots[i].present = 1u;
}

/* drop n_loss chunks per block (first n_loss slots of each block) */
static uint16_t drop_n_per_block(uint8_t n_loss) {
    uint16_t dropped = 0u;
    for (uint8_t l = 0u; l < FEC_LEVELS; l++)
        for (uint8_t b = 0u; b < FEC_BLOCKS_PER_LEVEL; b++)
            for (uint8_t i = 0u; i < n_loss; i++) {
                uint16_t pos = (uint16_t)(l * GEO_PYR_PHASE_LEN
                                         + b * FEC_CHUNKS_PER_BLOCK + i);
                ring.slots[pos].present = 0u;
                memset(store_work[pos].data, 0, TSTREAM_DATA_BYTES);
                store_work[pos].size = 0u;
                dropped++;
            }
    return dropped;
}

/* ── bench XOR encode ───────────────────────────── */
static double bench_xor_encode(void) {
    double total = 0.0;
    for (uint32_t iter = 0u; iter < BENCH_ITER; iter++) {
        reset_work();
        double t0 = now_sec();
        fec_encode_all(store_work, xor_parity);
        total += now_sec() - t0;
    }
    return (double)DATA_BYTES_TOTAL / (total / BENCH_ITER) / (1024.0 * 1024.0);
}

/* ── bench XOR recover (1 loss/block = 60 losses) ── */
static double bench_xor_recover(void) {
    /* pre-encode once */
    reset_work();
    fec_encode_all(store_work, xor_parity);

    double total = 0.0;
    for (uint32_t iter = 0u; iter < BENCH_ITER; iter++) {
        reset_work();
        /* re-encode into same parity (already done, reuse) */
        drop_n_per_block(1u); /* 1 loss/block → P0 path */
        double t0 = now_sec();
        fec_recover_all(&ring, store_work, xor_parity);
        total += now_sec() - t0;
    }
    /* recovered bytes = 60 × 4096 */
    double recovered_bytes = 60.0 * TSTREAM_DATA_BYTES;
    return recovered_bytes / (total / BENCH_ITER) / (1024.0 * 1024.0);
}

/* ── bench RS encode ────────────────────────────── */
static double bench_rs_encode(uint8_t fec_n) {
    double total = 0.0;
    for (uint32_t iter = 0u; iter < BENCH_ITER; iter++) {
        reset_work();
        double t0 = now_sec();
        fec_rs_encode_all(store_work, fec_n, rs_parity);
        total += now_sec() - t0;
    }
    return (double)DATA_BYTES_TOTAL / (total / BENCH_ITER) / (1024.0 * 1024.0);
}

/* ── bench RS recover (n_loss per block ≤ fec_n) ── */
static double bench_rs_recover(uint8_t fec_n, uint8_t n_loss) {
    /* pre-encode */
    reset_work();
    fec_rs_encode_all(store_work, fec_n, rs_parity);

    double total = 0.0;
    for (uint32_t iter = 0u; iter < BENCH_ITER; iter++) {
        reset_work();
        uint16_t dropped = drop_n_per_block(n_loss);
        double t0 = now_sec();
        fec_rs_recover_all(&ring, store_work, fec_n, rs_parity);
        total += now_sec() - t0;
        (void)dropped;
    }
    double recovered_bytes = (double)n_loss * FEC_BLOCKS_PER_LEVEL * FEC_LEVELS * TSTREAM_DATA_BYTES;
    return recovered_bytes / (total / BENCH_ITER) / (1024.0 * 1024.0);
}

/* ── parity overhead ────────────────────────────── */
static float parity_overhead_pct(uint8_t fec_n) {
    uint32_t parity_bytes = (uint32_t)FEC_LEVELS * FEC_BLOCKS_PER_LEVEL
                          * fec_n * TSTREAM_DATA_BYTES;
    return 100.0f * (float)parity_bytes / (float)DATA_BYTES_TOTAL;
}

/* ── print banner ───────────────────────────────── */
static void print_banner(void) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║        POGLS Phase 9c — FEC Throughput Benchmark            ║\n");
    printf("║  Data: %u chunks × %u B = %.2f MB                           ║\n",
           DATA_CHUNKS, TSTREAM_DATA_BYTES,
           (double)DATA_BYTES_TOTAL / (1024.0 * 1024.0));
    printf("║  Iterations: %u per config (averaged)                        ║\n", BENCH_ITER);
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");
}

int main(void) {
    gf256_init();
    fill_store();
    print_banner();

    /* ── Section 1: XOR baseline ── */
    printf("┌─ XOR FEC (Phase 9b baseline, fec_n=3 fixed) ─────────────────┐\n");
    double xor_enc = bench_xor_encode();
    double xor_rec = bench_xor_recover(); /* 1 loss/block */
    printf("│  encode:  %7.1f MB/s  (25%% parity overhead)                │\n", xor_enc);
    printf("│  recover: %7.1f MB/s  (1 loss/block → P0 path)             │\n", xor_rec);
    printf("│  2-loss coverage: 72%% (48/66 pairs per block)               │\n");
    printf("└───────────────────────────────────────────────────────────────┘\n\n");

    /* ── Section 2: RS sweep ── */
    printf("┌─ RS FEC (Phase 9c, flexible fec_n) ──────────────────────────┐\n");
    printf("│  %-6s  %-12s  %-13s  %-10s  %-8s  │\n",
           "fec_n", "encode MB/s", "recover MB/s", "loss/blk", "overhead");
    printf("│  ─────  ───────────  ────────────  ────────  ────────  │\n");

    uint8_t fec_ns[] = {1u, 2u, 3u, 4u, 6u, 8u, 12u};
    int n_configs = 7;

    for (int fi = 0; fi < n_configs; fi++) {
        uint8_t fn  = fec_ns[fi];
        uint8_t nl  = (fn > 1u) ? fn : 1u; /* recover at fec_n (worst case) */
        double enc  = bench_rs_encode(fn);
        double rec  = bench_rs_recover(fn, nl);
        float  oh   = parity_overhead_pct(fn);
        char   mark = (fn == 3u) ? '*' : ' '; /* highlight default */
        printf("│%c %-6u  %-12.1f  %-13.1f  %-10u  %.0f%%      %c  │\n",
               mark, fn, enc, rec, nl, oh, mark);
    }

    printf("│                                                               │\n");
    printf("│  * = Phase 9b default equivalent (fec_n=3)                   │\n");
    printf("└───────────────────────────────────────────────────────────────┘\n\n");

    /* ── Section 3: RS vs XOR head-to-head at fec_n=3 ── */
    printf("┌─ Head-to-Head: XOR vs RS at fec_n=3, 1 loss/block ───────────┐\n");
    double rs3_enc = bench_rs_encode(3u);
    double rs3_rec = bench_rs_recover(3u, 1u);
    printf("│              %-14s  %-14s                   │\n", "XOR", "RS fec_n=3");
    printf("│  encode:     %-14.1f  %-14.1f  MB/s         │\n", xor_enc, rs3_enc);
    printf("│  recover:    %-14.1f  %-14.1f  MB/s         │\n", xor_rec, rs3_rec);
    printf("│  cost ratio: 1.00×           %-6.2f×                         │\n",
           rs3_enc / xor_enc);
    printf("│  2-loss coverage: XOR=72%%    RS=100%%                         │\n");
    printf("└───────────────────────────────────────────────────────────────┘\n\n");

    /* ── Section 4: recover scaling with k ── */
    printf("┌─ RS Recover: fec_n=6, k-loss sweep (k=1..6) ─────────────────┐\n");
    printf("│  k-loss  recover MB/s  note                                   │\n");
    printf("│  ──────  ───────────  ─────                                   │\n");
    for (uint8_t k = 1u; k <= 6u; k++) {
        double r = bench_rs_recover(6u, k);
        printf("│  %-8u  %-11.1f  %s  │\n",
               k, r,
               k == 1u ? "single erasure                    " :
               k == 6u ? "boundary (k=fec_n)                " :
                         "                                  ");
    }
    printf("└───────────────────────────────────────────────────────────────┘\n\n");

    printf("Done.\n");
    return 0;
}
