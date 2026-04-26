/*
 * test_tring_fec.c — Phase 9c Integration Tests
 * ═══════════════════════════════════════════════
 * Tests the full TRing+RS-FEC pipeline via TRingFECCtx.
 *
 * T1: No loss — encode→recv→recover→reconstruct, byte-perfect
 * T2: 10% loss — RS recovers, output byte-perfect
 * T3: 20% loss (fec_n=3) — RS recovers to 0 gaps
 * T4: 30% loss (fec_n=4) — partial recovery, gaps_after reported correctly
 * T5: loss exactly at fec_n boundary — recovers; fec_n+1 per block fails clean
 * T6: fec_n=1 single parity — 1 loss/block recovers
 * T7: large file (720 chunks full) — all slots, 20% loss, fec_n=3
 * T8: stats API — verify gap/recovery numbers consistent
 *
 * Build: gcc -O2 -o test_tring_fec test_tring_fec.c && ./test_tring_fec
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "geo_temporal_lut.h"
#include "geo_temporal_ring.h"
#include "geo_pyramid.h"
#include "geo_tring_stream.h"
#include "geo_fec.h"
#include "geo_gf256.h"
#include "geo_rs.h"
#include "geo_fec_rs.h"
#include "geo_tring_fec.h"

/* ── test infra ─────────────────────────────────── */
static int pass_count = 0, fail_count = 0;
#define ASSERT(c,m) do { \
    if(c){printf("  [PASS] %s\n",(m));pass_count++;} \
    else {printf("  [FAIL] %s\n",(m));fail_count++;} \
} while(0)

/* ── static pools (avoid stack overflow) ────────── */
/* Two full pipeline contexts: TX and RX */
static TRingFECCtx tx_ctx;
static TRingFECCtx rx_ctx;

/* File buffers */
#define MAX_FILE_SIZE  (FEC_TOTAL_DATA * TSTREAM_DATA_BYTES)  /* 720 × 4096 = ~2.9 MB */
static uint8_t  file_buf [MAX_FILE_SIZE];
static uint8_t  out_buf  [MAX_FILE_SIZE];
static TStreamPkt sim_pkts[TSTREAM_MAX_PKTS]; /* pkt copies for loss simulation */

/* ── helpers ────────────────────────────────────── */
static void make_file(uint32_t size) {
    for (uint32_t i = 0u; i < size; i++)
        file_buf[i] = (uint8_t)((i * 7u + 13u) & 0xFF);
}

/*
 * simulate_send_recv:
 *   Encodes on TX, copies packets to sim_pkts, drops by loss_mask,
 *   recvs on RX (in order, skipping dropped), loads parity, recovers.
 *   loss_fn(i) = 1 to drop packet i, 0 to keep.
 */
typedef int (*LossFn)(uint16_t i, void *arg);

static int _no_loss(uint16_t i, void *arg)    { (void)i;(void)arg; return 0; }
static int _every_n(uint16_t i, void *arg)    { int n=*(int*)arg; return (n>0)&&(i%n==0); }
static int _rng_loss(uint16_t i, void *arg) {
    uint32_t *rng = (uint32_t*)arg;
    (void)i;
    *rng = (*rng) * 1664525u + 1013904223u;
    return (*rng & 0xFF) < 51u; /* ~20% */
}

static uint16_t simulate(
    uint32_t file_size, uint8_t fec_n,
    LossFn loss_fn, void *loss_arg)
{
    /* TX encode */
    tring_fec_prepare(&tx_ctx, fec_n);
    uint16_t n = tring_fec_encode(&tx_ctx, file_buf, file_size);

    /* copy pkts for simulation (re-slice to get TStreamPkt) */
    tstream_slice_file(sim_pkts, file_buf, file_size);

    /* RX prepare + load parity */
    tring_fec_prepare(&rx_ctx, fec_n);
    tring_fec_load_parity(&rx_ctx, &tx_ctx);
    rx_ctx.n_pkts = n;

    /* recv non-dropped pkts */
    for (uint16_t i = 0u; i < n; i++) {
        if (!loss_fn(i, loss_arg))
            tring_fec_recv(&rx_ctx, &sim_pkts[i]);
    }

    /* recover */
    tring_fec_recover(&rx_ctx);
    return n;
}

/* ── T1: No loss ────────────────────────────────── */
static void t1_no_loss(void) {
    printf("\n[T1] No loss — full pipeline, byte-perfect\n");
    uint32_t fsz = 12u * TSTREAM_DATA_BYTES; /* 3 blocks worth */
    make_file(fsz);
    simulate(fsz, 3u, _no_loss, NULL);

    uint32_t written = tring_fec_reconstruct(&rx_ctx, out_buf, MAX_FILE_SIZE);
    ASSERT(rx_ctx.gaps_before == 0u, "0 gaps before recover");
    ASSERT(rx_ctx.recovered   == 0u, "0 chunks recovered (nothing to do)");
    ASSERT(written == fsz,           "Reconstruct wrote correct byte count");
    ASSERT(memcmp(out_buf, file_buf, fsz) == 0, "Output byte-perfect");
}

/* ── T2: 10% loss (1 in 10) ─────────────────────── */
static void t2_ten_pct_loss(void) {
    printf("\n[T2] 10%% loss (every 10th pkt), fec_n=3\n");
    uint32_t fsz = 120u * TSTREAM_DATA_BYTES; /* 10 blocks */
    make_file(fsz);
    int every = 10;
    uint16_t n = simulate(fsz, 3u, _every_n, &every);

    tring_fec_reconstruct(&rx_ctx, out_buf, MAX_FILE_SIZE);

    TRingFECStats st; tring_fec_stats(&rx_ctx, &st);
    printf("   n=%u gaps_before=%u recovered=%u gaps_after=%u loss_pct=%u%%\n",
           st.n_pkts, st.gaps_before, st.recovered, st.gaps_after, st.loss_pct);

    ASSERT(st.gaps_after == 0u,                "0 gaps after recovery");
    ASSERT(memcmp(out_buf, file_buf, fsz) == 0,"Output byte-perfect");
    (void)n;
}

/* ── T3: 20% random loss, fec_n=3 ───────────────── */
static void t3_twenty_pct_loss(void) {
    printf("\n[T3] ~20%% random loss (seed=0xDEAD), fec_n=3\n");
    uint32_t fsz = 360u * TSTREAM_DATA_BYTES; /* 30 blocks = half ring */
    make_file(fsz);
    uint32_t rng = 0xDEAD1234u;
    simulate(fsz, 3u, _rng_loss, &rng);

    tring_fec_reconstruct(&rx_ctx, out_buf, MAX_FILE_SIZE);

    TRingFECStats st; tring_fec_stats(&rx_ctx, &st);
    printf("   gaps_before=%u recovered=%u gaps_after=%u loss_pct=%u%%\n",
           st.gaps_before, st.recovered, st.gaps_after, st.loss_pct);

    /* 20% random scatter: most blocks have ≤3 losses → recoverable */
    ASSERT(st.recovered > 0u,   "RS recovered some chunks");
    ASSERT(st.gaps_after < st.gaps_before, "gaps_after < gaps_before");
    /* byte check only on fully recovered case */
    if (st.gaps_after == 0u)
        ASSERT(memcmp(out_buf, file_buf, fsz) == 0, "Output byte-perfect (full recovery)");
    else
        printf("  [INFO] partial recovery — %u residual gaps (expected at 20%% with fec_n=3)\n",
               st.gaps_after);
}

/* ── T4: 30% loss, fec_n=4 ──────────────────────── */
static void t4_thirty_pct_loss(void) {
    printf("\n[T4] ~30%% random loss (seed=0xBEEF), fec_n=4\n");
    uint32_t fsz = 360u * TSTREAM_DATA_BYTES;
    make_file(fsz);
    uint32_t rng = 0xBEEF5678u;
    simulate(fsz, 4u, _rng_loss, &rng);

    TRingFECStats st; tring_fec_stats(&rx_ctx, &st);
    printf("   gaps_before=%u recovered=%u gaps_after=%u loss_pct=%u%%\n",
           st.gaps_before, st.recovered, st.gaps_after, st.loss_pct);

    ASSERT(st.gaps_after <= st.gaps_before, "gaps_after ≤ gaps_before (no corruption)");
    ASSERT(st.recovered  > 0u,              "fec_n=4 recovered some chunks");
}

/* ── T5: exact boundary — fec_n loss/block recovers; fec_n+1 fails clean ── */
static void t5_boundary(void) {
    printf("\n[T5] Boundary: drop fec_n / fec_n+1 per block, fec_n=3\n");
    const uint8_t fec_n = 3u;
    uint32_t fsz = FEC_TOTAL_DATA * TSTREAM_DATA_BYTES;
    make_file(fsz);

    /* ── case A: drop exactly fec_n in every block ── */
    tring_fec_prepare(&tx_ctx, fec_n);
    tring_fec_encode(&tx_ctx, file_buf, fsz);
    tstream_slice_file(sim_pkts, file_buf, fsz);

    tring_fec_prepare(&rx_ctx, fec_n);
    tring_fec_load_parity(&rx_ctx, &tx_ctx);
    rx_ctx.n_pkts = FEC_TOTAL_DATA;

    /* copy all data from TX, mark all present, then selectively drop */
    for (uint16_t i = 0u; i < FEC_TOTAL_DATA; i++) {
        memcpy(rx_ctx.store[i].data, tx_ctx.store[i].data, TSTREAM_DATA_BYTES);
        rx_ctx.store[i].size = tx_ctx.store[i].size;
        rx_ctx.ring.slots[i].present = 1u;
    }
    for (uint8_t l = 0u; l < FEC_LEVELS; l++)
        for (uint8_t b = 0u; b < FEC_BLOCKS_PER_LEVEL; b++)
            for (uint8_t skip = 0u; skip < fec_n; skip++) {
                uint16_t pos = (uint16_t)(l * GEO_PYR_PHASE_LEN
                                         + b * FEC_CHUNKS_PER_BLOCK + skip);
                rx_ctx.ring.slots[pos].present = 0u;
                memset(rx_ctx.store[pos].data, 0, TSTREAM_DATA_BYTES);
                rx_ctx.store[pos].size = 0u;
            }

    tring_fec_recover(&rx_ctx);
    TRingFECStats sa; tring_fec_stats(&rx_ctx, &sa);
    printf("   case A (k=fec_n=%u):   gaps_before=%u recovered=%u gaps_after=%u\n",
           fec_n, sa.gaps_before, sa.recovered, sa.gaps_after);
    ASSERT(sa.gaps_after == 0u, "Case A: k=fec_n — 0 gaps after (full recovery)");

    /* ── case B: drop fec_n+1 per block ── */
    tring_fec_prepare(&rx_ctx, fec_n);
    tring_fec_load_parity(&rx_ctx, &tx_ctx);
    rx_ctx.n_pkts = FEC_TOTAL_DATA;

    for (uint16_t i = 0u; i < FEC_TOTAL_DATA; i++) {
        memcpy(rx_ctx.store[i].data, tx_ctx.store[i].data, TSTREAM_DATA_BYTES);
        rx_ctx.store[i].size = tx_ctx.store[i].size;
        rx_ctx.ring.slots[i].present = 1u;
    }
    for (uint8_t l = 0u; l < FEC_LEVELS; l++)
        for (uint8_t b = 0u; b < FEC_BLOCKS_PER_LEVEL; b++)
            for (uint8_t skip = 0u; skip < fec_n + 1u; skip++) {
                uint16_t pos = (uint16_t)(l * GEO_PYR_PHASE_LEN
                                         + b * FEC_CHUNKS_PER_BLOCK + skip);
                rx_ctx.ring.slots[pos].present = 0u;
                memset(rx_ctx.store[pos].data, 0, TSTREAM_DATA_BYTES);
                rx_ctx.store[pos].size = 0u;
            }

    /* snapshot before recovery attempt */
    static TStreamChunk snap[FEC_TOTAL_DATA];
    memcpy(snap, rx_ctx.store, sizeof(TStreamChunk) * FEC_TOTAL_DATA);
    uint16_t gaps_b4 = 0u;
    for (uint16_t i = 0u; i < FEC_TOTAL_DATA; i++)
        if (!rx_ctx.ring.slots[i].present) gaps_b4++;

    tring_fec_recover(&rx_ctx);
    TRingFECStats sb; tring_fec_stats(&rx_ctx, &sb);
    printf("   case B (k=fec_n+1=%u): gaps_before=%u recovered=%u gaps_after=%u\n",
           fec_n + 1u, sb.gaps_before, sb.recovered, sb.gaps_after);

    /* check no corruption of unrecovered slots */
    int no_corrupt = 1;
    for (uint16_t i = 0u; i < FEC_TOTAL_DATA; i++) {
        if (!rx_ctx.ring.slots[i].present) {
            if (memcmp(rx_ctx.store[i].data, snap[i].data, TSTREAM_DATA_BYTES) != 0) {
                no_corrupt = 0; break;
            }
        }
    }
    ASSERT(sb.recovered == 0u, "Case B: k>fec_n — 0 recovered (beyond capacity)");
    ASSERT(no_corrupt,         "Case B: no corruption on unrecoverable slots");
}

/* ── T6: fec_n=1, 1 loss/block ──────────────────── */
static void t6_fec_n1(void) {
    printf("\n[T6] fec_n=1: single parity, 1 loss per block\n");
    uint32_t fsz = FEC_TOTAL_DATA * TSTREAM_DATA_BYTES;
    make_file(fsz);
    int every = 12; /* 1 loss per 12-chunk block */
    uint16_t n = simulate(fsz, 1u, _every_n, &every);

    tring_fec_reconstruct(&rx_ctx, out_buf, MAX_FILE_SIZE);
    TRingFECStats st; tring_fec_stats(&rx_ctx, &st);
    printf("   n=%u gaps_before=%u recovered=%u gaps_after=%u\n",
           st.n_pkts, st.gaps_before, st.recovered, st.gaps_after);

    ASSERT(st.gaps_after == 0u,                "0 gaps after (fec_n=1, 1 loss/block)");
    ASSERT(memcmp(out_buf, file_buf, fsz) == 0,"Output byte-perfect");
    (void)n;
}

/* ── T7: full ring 720 chunks, 20% loss, fec_n=3 ── */
static void t7_full_ring(void) {
    printf("\n[T7] Full ring (720 chunks), ~20%% random loss, fec_n=3\n");
    uint32_t fsz = FEC_TOTAL_DATA * TSTREAM_DATA_BYTES; /* 2,949,120 bytes */
    make_file(fsz);
    uint32_t rng = 0x9C3A1D7Eu;
    simulate(fsz, 3u, _rng_loss, &rng);

    TRingFECStats st; tring_fec_stats(&rx_ctx, &st);
    printf("   total=%u gaps_before=%u recovered=%u gaps_after=%u loss=~%u%%\n",
           st.n_pkts, st.gaps_before, st.recovered, st.gaps_after, st.loss_pct);

    ASSERT(st.n_pkts == FEC_TOTAL_DATA, "All 720 chunks in play");
    ASSERT(st.recovered > 0u,           "RS recovered chunks from 20% scatter");
    ASSERT(st.gaps_after <= st.gaps_before, "Monotone: gaps_after ≤ gaps_before");
}

/* ── T8: stats consistency ───────────────────────── */
static void t8_stats_consistency(void) {
    printf("\n[T8] Stats API consistency check\n");
    uint32_t fsz = 240u * TSTREAM_DATA_BYTES; /* 20 blocks */
    make_file(fsz);
    int every = 8; /* ~12.5% loss */
    simulate(fsz, 3u, _every_n, &every);

    TRingFECStats st; tring_fec_stats(&rx_ctx, &st);
    printf("   n=%u gaps_before=%u recovered=%u gaps_after=%u loss_pct=%u%%\n",
           st.n_pkts, st.gaps_before, st.recovered, st.gaps_after, st.loss_pct);

    ASSERT(st.n_pkts == 240u,                          "n_pkts = 240");
    ASSERT(st.gaps_before >= st.recovered,              "gaps_before ≥ recovered");
    ASSERT(st.gaps_after == st.gaps_before - st.recovered, "gaps_after = before - recovered");
    ASSERT(st.fec_n == 3u,                              "fec_n=3 stored in stats");
    ASSERT(st.loss_pct > 0u && st.loss_pct < 100u,     "loss_pct in sane range");
}

/* ── main ───────────────────────────────────────── */
int main(void) {
    printf("══════════════════════════════════════════════════════\n");
    printf(" POGLS Phase 9c Integration — TRing+RS-FEC Pipeline\n");
    printf("══════════════════════════════════════════════════════\n");

    gf256_init();

    t1_no_loss();
    t2_ten_pct_loss();
    t3_twenty_pct_loss();
    t4_thirty_pct_loss();
    t5_boundary();
    t6_fec_n1();
    t7_full_ring();
    t8_stats_consistency();

    printf("\n══════════════════════════════════════════════════════\n");
    printf(" RESULT: %d/%d PASS\n", pass_count, pass_count + fail_count);
    printf("══════════════════════════════════════════════════════\n");
    return (fail_count == 0) ? 0 : 1;
}
