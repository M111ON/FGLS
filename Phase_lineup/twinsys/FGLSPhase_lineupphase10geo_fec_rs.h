/*
 * geo_fec_rs.h — Phase 9c: RS backend for FEC
 * ═════════════════════════════════════════════
 * Drop-in complement to geo_fec.h.
 * Reuses FECParity struct unchanged. Adds RS encode/recover.
 *
 * fec_type = FEC_TYPE_RS  (value 2)
 * fec_n    = number of parity chunks per block (flexible, 1..12)
 *            tolerate any n_loss ≤ fec_n
 *
 * Parity layout: parity[block_global * fec_n + j], j=0..fec_n-1
 * NOTE: fec_n must be consistent across encode + recover calls.
 *       Total parity pool = 60 blocks × fec_n entries.
 *
 * API (RS):
 *   fec_rs_encode_block(store, l, b, fec_n, parity_pool, pool_stride)
 *   fec_rs_recover_block(r, store, par_block, fec_n)  → chunks recovered
 *   fec_rs_encode_all(store, fec_n, parity_pool)       → fills pool
 *   fec_rs_recover_all(r, store, fec_n, parity_pool)   → total recovered
 *
 * Internal: uses geo_rs.h byte-serial Vandermonde RS over GF(256).
 */
#ifndef GEO_FEC_RS_H
#define GEO_FEC_RS_H

#include <stdint.h>
#include <string.h>
#include "geo_tring_stream.h"
#include "geo_temporal_ring.h"
#include "geo_fec.h"
#include "geo_rs.h"

/* ── helper: gather/scatter between TStreamChunk[] and uint8_t[][] ── */
/* data_buf[k][TSTREAM_DATA_BYTES], present[k] ← from store + ring     */
static inline void _fec_rs_gather(
    const TStreamChunk  store[FEC_TOTAL_DATA],
    const TRingCtx     *r,
    uint16_t            base,
    uint8_t             data_buf[][TSTREAM_DATA_BYTES],
    uint8_t             present[RS_MAX_K],
    uint16_t            chunk_sz[RS_MAX_K])
{
    for (uint8_t i = 0u; i < FEC_CHUNKS_PER_BLOCK; i++) {
        uint16_t pos = (uint16_t)(base + i);
        present[i]   = r->slots[pos].present;
        chunk_sz[i]  = store[pos].size;
        if (present[i])
            memcpy(data_buf[i], store[pos].data, TSTREAM_DATA_BYTES);
        else
            memset(data_buf[i], 0, TSTREAM_DATA_BYTES);
    }
}

static inline void _fec_rs_scatter(
    TStreamChunk  store[FEC_TOTAL_DATA],
    TRingCtx     *r,
    uint16_t      base,
    const uint8_t data_buf[][TSTREAM_DATA_BYTES],
    const uint8_t present_before[RS_MAX_K],
    const uint16_t chunk_sz[RS_MAX_K])
{
    for (uint8_t i = 0u; i < FEC_CHUNKS_PER_BLOCK; i++) {
        if (present_before[i]) continue; /* already had data */
        uint16_t pos = (uint16_t)(base + i);
        uint16_t sz  = chunk_sz[i];
        memcpy(store[pos].data, data_buf[i], sz);
        memset(store[pos].data + sz, 0, TSTREAM_DATA_BYTES - sz);
        store[pos].size       = sz;
        r->slots[pos].present = 1u;
    }
}

/* ── RS encode one block ────────────────────────── */
/*
 * parity_block[fec_n] — caller provides pointer to the fec_n FECParity
 *   slots for this block.
 */
static inline void fec_rs_encode_block(
    const TStreamChunk  store[FEC_TOTAL_DATA],
    uint8_t             level,
    uint8_t             block,
    uint8_t             fec_n,
    FECParity           parity_block[]) /* [fec_n] entries */
{
    gf256_init();

    uint16_t base = (uint16_t)(level * GEO_PYR_PHASE_LEN
                              + block * FEC_CHUNKS_PER_BLOCK);
    const uint8_t k = FEC_CHUNKS_PER_BLOCK;

    /* gather chunk pointers (const cast — encode reads only) */
    const uint8_t *data_ptrs[RS_MAX_K];
    uint16_t chunk_sz[RS_MAX_K];
    uint16_t max_sz = 0u;
    for (uint8_t i = 0u; i < k; i++) {
        data_ptrs[i] = store[base + i].data;
        chunk_sz[i]  = store[base + i].size;
        if (chunk_sz[i] > max_sz) max_sz = chunk_sz[i];
    }

    /* encode: parity_out[j][pos] per byte position */
    for (uint8_t j = 0u; j < fec_n; j++) {
        memset(parity_block[j].data, 0, TSTREAM_DATA_BYTES);
        parity_block[j].fec_type   = FEC_TYPE_RS;
        parity_block[j].fec_n      = fec_n;
        parity_block[j].level      = level;
        parity_block[j].block      = block;
        parity_block[j].parity_idx = j;
        /* store chunk sizes so recover knows actual sizes of missing chunks */
        memcpy(parity_block[j].chunk_sizes, chunk_sz, sizeof(chunk_sz));
    }

    uint8_t data_col[RS_MAX_K];
    uint8_t par_col [RS_MAX_N];

    for (uint16_t pos = 0u; pos < max_sz; pos++) {
        for (uint8_t i = 0u; i < k; i++)
            data_col[i] = (pos < chunk_sz[i]) ? data_ptrs[i][pos] : 0u;

        rs_encode_byte(data_col, k, fec_n, par_col);

        for (uint8_t j = 0u; j < fec_n; j++)
            parity_block[j].data[pos] = par_col[j];
    }
}

/* ── RS recover one block ───────────────────────── */
/*
 * par_block[fec_n] — the fec_n parity entries for this block.
 * Returns number of chunks recovered (0 if no gaps, or unrecoverable).
 * On unrecoverable: store/ring left unmodified (no corruption).
 */
static inline uint8_t fec_rs_recover_block(
    TRingCtx          *r,
    TStreamChunk       store[FEC_TOTAL_DATA],
    const FECParity    par_block[], /* [fec_n] */
    uint8_t            fec_n)
{
    uint16_t base = (uint16_t)(par_block[0].level * GEO_PYR_PHASE_LEN
                               + par_block[0].block * FEC_CHUNKS_PER_BLOCK);
    const uint8_t k = FEC_CHUNKS_PER_BLOCK;

    /* count losses */
    uint8_t n_erase = 0u;
    for (uint8_t i = 0u; i < k; i++)
        if (!r->slots[base + i].present) n_erase++;

    if (n_erase == 0u) return 0u;           /* nothing to do */
    if (n_erase > fec_n) return 0u;         /* beyond capacity — fail clean */

    /* gather data into buffers (static global — single-threaded, no recursion) */
    static uint8_t data_buf[RS_MAX_K][TSTREAM_DATA_BYTES];
    uint8_t  present[RS_MAX_K];
    uint16_t chunk_sz[RS_MAX_K];

    _fec_rs_gather(store, r, base, data_buf, present, chunk_sz);

    /* chunk_sz for missing slots comes from parity (all parity carry same sizes) */
    for (uint8_t i = 0u; i < k; i++)
        if (!present[i]) chunk_sz[i] = par_block[0].chunk_sizes[i];

    /* copy parity data into flat 2D buffer for rs_recover */
    static uint8_t parity_buf[RS_MAX_N][TSTREAM_DATA_BYTES];
    uint16_t parity_sz = 0u;
    for (uint8_t j = 0u; j < fec_n; j++) {
        memcpy(parity_buf[j], par_block[j].data, TSTREAM_DATA_BYTES);
    }
    /* parity_sz = max chunk_sz */
    for (uint8_t i = 0u; i < k; i++)
        if (chunk_sz[i] > parity_sz) parity_sz = chunk_sz[i];

    int ok = rs_recover(
        data_buf, present, chunk_sz, k,
        (const uint8_t (*)[TSTREAM_DATA_BYTES])parity_buf,
        parity_sz, fec_n);

    if (!ok) return 0u; /* unrecoverable */

    /* scatter recovered chunks back */
    _fec_rs_scatter(store, r, base, (const uint8_t (*)[TSTREAM_DATA_BYTES])data_buf,
                    present, chunk_sz);

    return n_erase; /* all erasures were recovered */
}

/* ── RS encode all 60 blocks ────────────────────── */
/*
 * parity_pool: caller allocates FEC_LEVELS * FEC_BLOCKS_PER_LEVEL * fec_n entries.
 * Indexing: parity_pool[(l*12+b)*fec_n + j]
 */
static inline void fec_rs_encode_all(
    const TStreamChunk store[FEC_TOTAL_DATA],
    uint8_t            fec_n,
    FECParity          parity_pool[]) /* [60 * fec_n] */
{
    for (uint8_t l = 0u; l < FEC_LEVELS; l++)
        for (uint8_t b = 0u; b < FEC_BLOCKS_PER_LEVEL; b++) {
            uint8_t idx = (uint8_t)(l * FEC_BLOCKS_PER_LEVEL + b);
            fec_rs_encode_block(store, l, b, fec_n,
                                &parity_pool[idx * fec_n]);
        }
}

/* ── RS recover all 60 blocks ───────────────────── */
static inline uint16_t fec_rs_recover_all(
    TRingCtx         *r,
    TStreamChunk      store[FEC_TOTAL_DATA],
    uint8_t           fec_n,
    const FECParity   parity_pool[]) /* [60 * fec_n] */
{
    uint16_t total = 0u;
    for (uint8_t l = 0u; l < FEC_LEVELS; l++)
        for (uint8_t b = 0u; b < FEC_BLOCKS_PER_LEVEL; b++) {
            uint8_t idx = (uint8_t)(l * FEC_BLOCKS_PER_LEVEL + b);
            total += fec_rs_recover_block(r, store,
                                          &parity_pool[idx * fec_n],
                                          fec_n);
        }
    return total;
}

/* ══════════════════════════════════════════════════════════════════
 * fec_hybrid_recover_block — L1→L2→L3 cascaded recovery
 *
 * Strategy per block:
 *   L1 XOR  : attempt via existing fec_xor_recover_block()
 *             fast (443 MB/s), handles 72% of loss patterns
 *   L2 Rewind : for remaining gaps — fill from RewindBuffer
 *               zero compute (memcpy only), covers temporal scatter
 *   L3 RS    : only if still missing after L2
 *              100% coverage, ~10 MB/s — safety net, rarely fires
 *
 * Returns: total chunks recovered across all three layers.
 * ══════════════════════════════════════════════════════════════════ */
#include "geo_rewind.h"
#include "geo_fec.h"   /* fec_xor_recover_block, FEC_PARITY_PER_BLOCK */

static inline uint16_t fec_hybrid_recover_block(
    TRingCtx           *r,
    TStreamChunk        store[FEC_TOTAL_DATA],
    uint8_t             level,
    uint8_t             block,
    const FECParity     xor_par[FEC_PARITY_PER_BLOCK], /* 3 XOR parities */
    const FECParity     rs_par[],                       /* [fec_n] RS parities */
    uint8_t             fec_n,
    RewindBuffer       *rewind)                         /* L2 temporal pool */
{
    uint16_t base  = (uint16_t)(level * GEO_PYR_PHASE_LEN
                                + block * FEC_CHUNKS_PER_BLOCK);
    uint16_t total = 0u;

    /* record which slots were originally missing (for L3 guard) */
    uint8_t was_missing[FEC_CHUNKS_PER_BLOCK];
    uint8_t n_orig_miss = 0u;
    for (uint8_t i = 0u; i < FEC_CHUNKS_PER_BLOCK; i++) {
        was_missing[i] = (uint8_t)(!r->slots[(uint16_t)(base+i)].present);
        if (was_missing[i]) n_orig_miss++;
    }
    if (n_orig_miss == 0u) return 0u;

    /* ── L2: Rewind fill first (exact data, zero compute) ─ */
    if (rewind) {
        for (uint8_t i = 0u; i < FEC_CHUNKS_PER_BLOCK; i++) {
            if (!was_missing[i]) continue;
            uint16_t pos = (uint16_t)(base + i);
            if (r->slots[pos].present) continue;
            const TStreamChunk *src = rewind_find(rewind, GEO_WALK[pos]);
            if (!src) continue;
            store[pos] = *src;
            r->slots[pos].present = 1u;
            total++;
        }
    }

    /* ── L1: XOR for slots rewind couldn't fill ─────────── */
    total += fec_recover_block(r, store, xor_par);

    /* ── L3: RS safety net ──────────────────────────────
     * L1 may write wrong data (bad parity) into originally-missing slots.
     * Undo all L1 writes to originally-missing slots, then let RS decide.
     * L2-filled slots (exact data) are kept — their was_missing=1 but
     * present=1 AND data is trusted, so we only undo slots L1 touched
     * that L2 did NOT already fill. */
    uint8_t l3_needed = 0u;
    for (uint8_t i = 0u; i < FEC_CHUNKS_PER_BLOCK; i++) {
        /* only care about slots that were missing AND not filled by L2 */
        if (!was_missing[i]) continue;
        uint16_t pos = (uint16_t)(base + i);
        uint8_t filled_by_l2 = 0u;
        if (rewind && r->slots[pos].present) {
            /* check if rewind has this enc — if so, L2 filled it */
            filled_by_l2 = rewind_has(rewind, GEO_WALK[pos]);
        }
        if (!filled_by_l2) l3_needed++;
    }

    if (l3_needed > 0u && l3_needed <= fec_n) {
        /* undo L1 writes (not L2 writes) to originally-missing slots */
        for (uint8_t i = 0u; i < FEC_CHUNKS_PER_BLOCK; i++) {
            if (!was_missing[i]) continue;
            uint16_t pos = (uint16_t)(base + i);
            if (!rewind || !rewind_has(rewind, GEO_WALK[pos])) {
                if (r->slots[pos].present) {
                    r->slots[pos].present = 0u;
                    memset(store[pos].data, 0, TSTREAM_DATA_BYTES);
                    total--;
                }
            }
        }
        total += fec_rs_recover_block(r, store, rs_par, fec_n);
    }

    return total;
}

/* ── apply hybrid recovery to all 60 blocks ─── */
static inline uint16_t fec_hybrid_recover_all(
    TRingCtx           *r,
    TStreamChunk        store[FEC_TOTAL_DATA],
    uint8_t             fec_n,
    const FECParity     xor_parity[FEC_TOTAL_PARITY],  /* 180 XOR parities */
    const FECParity     rs_parity_pool[],               /* [60*fec_n] */
    RewindBuffer       *rewind)
{
    uint16_t total = 0u;
    for (uint8_t l = 0u; l < FEC_LEVELS; l++)
        for (uint8_t b = 0u; b < FEC_BLOCKS_PER_LEVEL; b++) {
            uint8_t xor_idx = (uint8_t)(l * FEC_BLOCKS_PER_LEVEL + b);
            uint8_t rs_idx  = xor_idx;
            total += fec_hybrid_recover_block(
                r, store, l, b,
                &xor_parity[xor_idx * FEC_PARITY_PER_BLOCK],
                &rs_parity_pool[(uint16_t)rs_idx * fec_n],
                fec_n, rewind);
        }
    return total;
}

#endif /* GEO_FEC_RS_H */
