/*
 * bench_hybrid.c — Phase 10: Hybrid L2+L3 Recovery Benchmark
 * ═══════════════════════════════════════════════════════════
 * Measures:
 *   1. L2 Rewind hit rate vs loss pattern (random / burst / stride)
 *   2. Hybrid recover throughput vs RS-only
 *   3. L3 RS fire rate (how often safety net actually needed)
 *   4. End-to-end tring_fec pipeline throughput
 *
 * Loss patterns:
 *   RANDOM  — uniform random across all slots
 *   BURST   — consecutive slots within a block
 *   STRIDE  — same index in every block (worst case for XOR)
 *
 * Build: gcc -O2 -o bench_hybrid bench_hybrid.c && ./bench_hybrid
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "geo_temporal_lut.h"
#include "geo_temporal_ring.h"
#include "geo_pyramid.h"
#include "geo_tring_stream.h"
#include "geo_fec.h"
#include "geo_gf256.h"
#include "geo_rs.h"
#include "geo_fec_rs.h"   /* includes geo_rewind.h + hybrid */
#include "geo_tring_fec.h"

/* ── config ─────────────────────────────────────── */
#define BENCH_ITER        8u
#define FEC_N_DEFAULT     3u
#define DATA_CHUNKS       FEC_TOTAL_DATA
#define DATA_BYTES_TOTAL  ((uint32_t)DATA_CHUNKS * TSTREAM_DATA_BYTES)

/* ── static pools ───────────────────────────────── */
static TStreamChunk store_orig[DATA_CHUNKS];
static TStreamChunk store_work[DATA_CHUNKS];
static FECParity    rs_parity[FEC_LEVELS * FEC_BLOCKS_PER_LEVEL * RS_MAX_N];
static FECParity    xor_parity[FEC_TOTAL_PARITY];
static TRingCtx     ring;
static RewindBuffer rewind_buf;

/* ── timer ──────────────────────────────────────── */
static inline double now_sec(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* ── setup ──────────────────────────────────────── */
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

static void reset_rewind(void) {
    rewind_init(&rewind_buf);
    /* feed all current store_orig chunks into rewind */
    for (uint16_t i = 0u; i < DATA_CHUNKS; i++)
        rewind_store(&rewind_buf, GEO_WALK[i], &store_orig[i]);
}

/* ── loss patterns ──────────────────────────────── */
typedef enum { LOSS_RANDOM = 0, LOSS_BURST, LOSS_STRIDE } LossPattern;

/* drop exactly n_loss per block according to pattern
 * returns total dropped */
static uint16_t drop_pattern(uint8_t n_loss, LossPattern pat, uint32_t seed) {
    uint16_t dropped = 0u;
    uint32_t rng = seed;

    for (uint8_t l = 0u; l < FEC_LEVELS; l++) {
        for (uint8_t b = 0u; b < FEC_BLOCKS_PER_LEVEL; b++) {
            uint16_t base = (uint16_t)(l * GEO_PYR_PHASE_LEN
                                       + b * FEC_CHUNKS_PER_BLOCK);
            uint8_t chosen[FEC_CHUNKS_PER_BLOCK];
            memset(chosen, 0, sizeof(chosen));

            switch (pat) {
            case LOSS_BURST:
                /* consecutive slots starting at random offset */
                rng = rng * 1664525u + 1013904223u;
                uint8_t start = (uint8_t)(rng % FEC_CHUNKS_PER_BLOCK);
                for (uint8_t i = 0u; i < n_loss; i++)
                    chosen[(start + i) % FEC_CHUNKS_PER_BLOCK] = 1u;
                break;

            case LOSS_STRIDE:
                /* same slot index in every block (worst case for XOR) */
                for (uint8_t i = 0u; i < n_loss; i++)
                    chosen[i % FEC_CHUNKS_PER_BLOCK] = 1u;
                break;

            case LOSS_RANDOM:
            default: {
                /* fisher-yates pick n_loss from k */
                uint8_t idx[FEC_CHUNKS_PER_BLOCK];
                for (uint8_t i = 0u; i < FEC_CHUNKS_PER_BLOCK; i++) idx[i] = i;
                for (uint8_t i = 0u; i < n_loss; i++) {
                    rng = rng * 1664525u + 1013904223u;
                    uint8_t j = (uint8_t)(i + rng % (FEC_CHUNKS_PER_BLOCK - i));
                    uint8_t tmp = idx[i]; idx[i] = idx[j]; idx[j] = tmp;
                    chosen[idx[i]] = 1u;
                }
                break;
            }
            }

            for (uint8_t i = 0u; i < FEC_CHUNKS_PER_BLOCK; i++) {
                if (!chosen[i]) continue;
                uint16_t pos = (uint16_t)(base + i);
                ring.slots[pos].present = 0u;
                memset(store_work[pos].data, 0, TSTREAM_DATA_BYTES);
                store_work[pos].size = 0u;
                dropped++;
            }
        }
    }
    return dropped;
}

/* ── L2 hit rate measurement ────────────────────── */
typedef struct {
    uint16_t total_missing;
    uint16_t l2_hits;       /* filled by rewind */
    uint16_t l3_needed;     /* still missing after L2 */
} HitStats;

static HitStats measure_l2_hits(void) {
    HitStats st = {0, 0, 0};
    for (uint16_t i = 0u; i < DATA_CHUNKS; i++) {
        if (ring.slots[i].present) continue;
        st.total_missing++;
        if (rewind_has(&rewind_buf, GEO_WALK[i]))
            st.l2_hits++;
        else
            st.l3_needed++;
    }
    return st;
}

/* ── bench hybrid recover ───────────────────────── */
static double bench_hybrid_recover(uint8_t fec_n, uint8_t n_loss,
                                    LossPattern pat, uint32_t seed) {
    reset_work();
    fec_rs_encode_all(store_work, fec_n, rs_parity);
    memset(xor_parity, 0, sizeof(xor_parity)); /* L1 disabled */

    double total = 0.0;
    for (uint32_t iter = 0u; iter < BENCH_ITER; iter++) {
        reset_work();
        reset_rewind();
        drop_pattern(n_loss, pat, seed + iter);
        double t0 = now_sec();
        fec_hybrid_recover_all(&ring, store_work, fec_n,
                               xor_parity, rs_parity, &rewind_buf);
        total += now_sec() - t0;
    }
    double rec_bytes = (double)n_loss * FEC_LEVELS * FEC_BLOCKS_PER_LEVEL
                     * TSTREAM_DATA_BYTES;
    return rec_bytes / (total / BENCH_ITER) / (1024.0 * 1024.0);
}

/* ── bench RS-only recover (baseline) ──────────── */
static double bench_rs_only_recover(uint8_t fec_n, uint8_t n_loss,
                                     LossPattern pat, uint32_t seed) {
    reset_work();
    fec_rs_encode_all(store_work, fec_n, rs_parity);

    double total = 0.0;
    for (uint32_t iter = 0u; iter < BENCH_ITER; iter++) {
        reset_work();
        drop_pattern(n_loss, pat, seed + iter);
        double t0 = now_sec();
        fec_rs_recover_all(&ring, store_work, fec_n, rs_parity);
        total += now_sec() - t0;
    }
    double rec_bytes = (double)n_loss * FEC_LEVELS * FEC_BLOCKS_PER_LEVEL
                     * TSTREAM_DATA_BYTES;
    return rec_bytes / (total / BENCH_ITER) / (1024.0 * 1024.0);
}

/* ── tring_fec end-to-end pipeline bench ────────── */
static double bench_pipeline_encode(uint8_t fec_n) {
    static TRingFECCtx ctx;
    static uint8_t file_data[DATA_BYTES_TOTAL];
    memset(file_data, 0xA5, sizeof(file_data));

    double total = 0.0;
    for (uint32_t iter = 0u; iter < BENCH_ITER; iter++) {
        tring_fec_prepare(&ctx, fec_n);
        double t0 = now_sec();
        tring_fec_encode(&ctx, file_data, sizeof(file_data));
        total += now_sec() - t0;
    }
    return (double)DATA_BYTES_TOTAL / (total / BENCH_ITER) / (1024.0 * 1024.0);
}

static double bench_pipeline_recover(uint8_t fec_n, uint8_t n_loss,
                                      LossPattern pat) {
    static TRingFECCtx tx, rx;
    static uint8_t file_data[DATA_BYTES_TOTAL];
    static uint8_t out_buf[DATA_BYTES_TOTAL];
    memset(file_data, 0xB7, sizeof(file_data));

    /* encode once */
    tring_fec_prepare(&tx, fec_n);
    tring_fec_encode(&tx, file_data, sizeof(file_data));

    double total = 0.0;
    for (uint32_t iter = 0u; iter < BENCH_ITER; iter++) {
        tring_fec_prepare(&rx, fec_n);
        tring_fec_load_parity(&rx, &tx);

        /* simulate recv with loss */
        for (uint16_t i = 0u; i < tx.n_pkts; i++) {
            /* apply loss: skip n_loss per block (burst from slot 0) */
            uint8_t b_slot = (uint8_t)(i % FEC_CHUNKS_PER_BLOCK);
            if (b_slot < n_loss) continue; /* drop */
            TStreamPkt pkt;
            pkt.enc   = GEO_WALK[i];
            pkt.size  = tx.store[i].size;
            pkt.crc16 = _tstream_crc16(tx.store[i].data, tx.store[i].size);
            memcpy(pkt.data, tx.store[i].data, tx.store[i].size);
            tring_fec_recv(&rx, &pkt);
        }

        double t0 = now_sec();
        tring_fec_recover(&rx);
        total += now_sec() - t0;
        tring_fec_reconstruct(&rx, out_buf, sizeof(out_buf));
    }
    double rec_bytes = (double)n_loss * FEC_LEVELS * FEC_BLOCKS_PER_LEVEL
                     * TSTREAM_DATA_BYTES;
    return rec_bytes / (total / BENCH_ITER) / (1024.0 * 1024.0);
}

/* ── helpers ────────────────────────────────────── */
static const char *pat_name(LossPattern p) {
    switch (p) {
    case LOSS_RANDOM: return "random";
    case LOSS_BURST:  return "burst ";
    case LOSS_STRIDE: return "stride";
    default:          return "???   ";
    }
}

int main(void) {
    gf256_init();
    fill_store();
    srand(42);

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║      POGLS Phase 10 — Hybrid L2+L3 Recovery Benchmark      ║\n");
    printf("║  Data: %u chunks × %u B = %.2f MB                         ║\n",
           DATA_CHUNKS, TSTREAM_DATA_BYTES,
           (double)DATA_BYTES_TOTAL / (1024.0 * 1024.0));
    printf("║  Iterations: %u per config | fec_n=%u                        ║\n",
           BENCH_ITER, FEC_N_DEFAULT);
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");

    /* ═══════════════════════════════════════════════
     * Section 1: L2 Rewind hit rate vs loss pattern
     * ═══════════════════════════════════════════════ */
    printf("┌─ Section 1: L2 Rewind Hit Rate vs Loss Pattern ──────────────┐\n");
    printf("│  pattern  n_loss  total_miss  l2_hits  l3_needed  hit_pct   │\n");
    printf("│  ───────  ──────  ──────────  ───────  ─────────  ───────   │\n");

    LossPattern patterns[] = {LOSS_RANDOM, LOSS_BURST, LOSS_STRIDE};
    uint8_t     loss_vals[] = {1u, 2u, 3u};

    for (int pi = 0; pi < 3; pi++) {
        for (int li = 0; li < 3; li++) {
            uint8_t nl = loss_vals[li];
            reset_work();
            reset_rewind();
            drop_pattern(nl, patterns[pi], 0xDEADBEEF);
            HitStats hs = measure_l2_hits();
            float hit_pct = hs.total_missing
                ? 100.0f * hs.l2_hits / hs.total_missing : 100.0f;
            printf("│  %-7s  %-6u  %-10u  %-7u  %-9u  %5.1f%%    │\n",
                   pat_name(patterns[pi]), nl,
                   hs.total_missing, hs.l2_hits, hs.l3_needed, hit_pct);
        }
    }
    printf("└───────────────────────────────────────────────────────────────┘\n\n");

    /* ═══════════════════════════════════════════════
     * Section 2: Hybrid vs RS-only throughput
     * ═══════════════════════════════════════════════ */
    printf("┌─ Section 2: Hybrid vs RS-Only Recover (MB/s) ────────────────┐\n");
    printf("│  pattern  n_loss  hybrid MB/s  rs-only MB/s  speedup        │\n");
    printf("│  ───────  ──────  ──────────   ────────────  ───────        │\n");

    for (int pi = 0; pi < 3; pi++) {
        for (int li = 0; li < 3; li++) {
            uint8_t nl = loss_vals[li];
            if (nl > FEC_N_DEFAULT) continue;
            double hyb = bench_hybrid_recover(FEC_N_DEFAULT, nl,
                                               patterns[pi], 0xCAFE + pi*10 + li);
            double rs  = bench_rs_only_recover(FEC_N_DEFAULT, nl,
                                                patterns[pi], 0xCAFE + pi*10 + li);
            double speedup = (rs > 0.0) ? hyb / rs : 0.0;
            printf("│  %-7s  %-6u  %-12.1f  %-12.1f  %5.2f×         │\n",
                   pat_name(patterns[pi]), nl, hyb, rs, speedup);
        }
    }
    printf("└───────────────────────────────────────────────────────────────┘\n\n");

    /* ═══════════════════════════════════════════════
     * Section 3: L3 RS fire rate
     * rewind evicted = L3 must fire
     * ═══════════════════════════════════════════════ */
    printf("┌─ Section 3: L3 RS Fire Rate (rewind full vs evicted) ────────┐\n");
    printf("│  scenario         n_loss  l2_hits  l3_fires  l3_fire_pct   │\n");
    printf("│  ───────────────  ──────  ───────  ────────  ───────────   │\n");

    /* full rewind */
    for (uint8_t nl = 1u; nl <= FEC_N_DEFAULT; nl++) {
        reset_work();
        reset_rewind(); /* rewind has everything */
        drop_pattern(nl, LOSS_RANDOM, 0xBEEF);
        HitStats hs = measure_l2_hits();
        float l3_pct = hs.total_missing
            ? 100.0f * hs.l3_needed / hs.total_missing : 0.0f;
        printf("│  rewind=full      %-6u  %-7u  %-8u  %5.1f%%        │\n",
               nl, hs.l2_hits, hs.l3_needed, l3_pct);
    }

    /* evicted rewind (overflow by 2×) */
    TStreamChunk dummy; memset(&dummy, 0xDD, sizeof(dummy));
    rewind_init(&rewind_buf);
    for (uint32_t i = 0u; i < REWIND_SLOTS * 2u; i++)
        rewind_store(&rewind_buf, 0xF000u + i, &dummy);

    for (uint8_t nl = 1u; nl <= FEC_N_DEFAULT; nl++) {
        reset_work();
        drop_pattern(nl, LOSS_RANDOM, 0xBEEF);
        HitStats hs = measure_l2_hits();
        float l3_pct = hs.total_missing
            ? 100.0f * hs.l3_needed / hs.total_missing : 0.0f;
        printf("│  rewind=evicted   %-6u  %-7u  %-8u  %5.1f%%        │\n",
               nl, hs.l2_hits, hs.l3_needed, l3_pct);
    }
    printf("└───────────────────────────────────────────────────────────────┘\n\n");

    /* ═══════════════════════════════════════════════
     * Section 4: End-to-end tring_fec pipeline
     * ═══════════════════════════════════════════════ */
    printf("┌─ Section 4: End-to-End tring_fec Pipeline ───────────────────┐\n");
    printf("│  fec_n  encode MB/s  recover MB/s (1-loss burst)            │\n");
    printf("│  ─────  ──────────  ──────────────────────────              │\n");

    uint8_t fec_ns[] = {1u, 2u, 3u, 4u, 6u};
    for (int fi = 0; fi < 5; fi++) {
        uint8_t fn = fec_ns[fi];
        double enc = bench_pipeline_encode(fn);
        double rec = bench_pipeline_recover(fn, 1u, LOSS_BURST);
        char mark = (fn == FEC_N_DEFAULT) ? '*' : ' ';
        printf("│%c %-5u  %-12.1f  %-12.1f                            %c │\n",
               mark, fn, enc, rec, mark);
    }
    printf("│  * = default fec_n                                           │\n");
    printf("└───────────────────────────────────────────────────────────────┘\n\n");

    printf("Done.\n");
    return 0;
}
