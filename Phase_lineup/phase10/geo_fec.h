/*
 * geo_fec.h — Phase 9b: XOR Mini-Block FEC (stride-3 A+B)
 * ══════════════════════════════════════════════════════════
 * Layout:
 *   720 data → 5 levels → 12 blocks/level → 12 chunks/block
 *   Per block: 3 parity chunks (overhead 25%)
 *
 *   P0 idx=0: XOR(all 12)          — detects/recovers any 1 loss
 *   P1 idx=1: XOR(0,3,6,9)         — stride-3 phase-0
 *   P2 idx=2: XOR(1,4,7,10)        — stride-3 phase-1
 *   (phase-2 = {2,5,8,11} implicit in P0 XOR P1 XOR P2)
 *
 * 2-loss recovery rate: 48/66 pairs = 72% (10-seed avg +5.6 vs odd/even)
 * Unrecoverable 2-loss: same-stride-2 pairs only ({2,5,8,11} choose 2 = 6 pairs)
 *   plus cross-pairs where all 3 equations are dependent (12 pairs)
 *
 * fec_type / fec_n fields reserved for Phase 9c (RS backend swap)
 * ══════════════════════════════════════════════════════════
 */
#ifndef GEO_FEC_H
#define GEO_FEC_H

#include <stdint.h>
#include <string.h>
#include "geo_tring_stream.h"

/* ── layout ─────────────────────────────────────── */
#define FEC_CHUNKS_PER_BLOCK   12u
#define FEC_BLOCKS_PER_LEVEL   12u
#define FEC_LEVELS              5u
#define FEC_PARITY_PER_BLOCK    3u   /* 9b: P0=all, P1=stride3A, P2=stride3B */
#define FEC_TOTAL_PARITY       (FEC_LEVELS * FEC_BLOCKS_PER_LEVEL * FEC_PARITY_PER_BLOCK) /* 180 */
#define FEC_TOTAL_DATA         720u

typedef char _fec_block_assert[
    (FEC_CHUNKS_PER_BLOCK * FEC_BLOCKS_PER_LEVEL == GEO_PYR_PHASE_LEN) ? 1 : -1];
typedef char _fec_parity_assert[(FEC_TOTAL_PARITY == 180u) ? 1 : -1];

/* ── parity type tags (future-proof for 9c RS swap) */
#define FEC_TYPE_DATA   0u
#define FEC_TYPE_XOR    1u
#define FEC_TYPE_RS     2u   /* reserved Phase 9c */

/* ── stride-3 masks (compile-time constants) ─────
 * P0: indices covered = all  → mask implicit (always 1)
 * P1: 0,3,6,9   → bit pattern 100100100100
 * P2: 1,4,7,10  → bit pattern 010010010010
 * P3 (implicit): 2,5,8,11  = P0 XOR P1 XOR P2 */
static const uint8_t FEC_S3A[FEC_CHUNKS_PER_BLOCK] = {1,0,0,1,0,0,1,0,0,1,0,0};
static const uint8_t FEC_S3B[FEC_CHUNKS_PER_BLOCK] = {0,1,0,0,1,0,0,1,0,0,1,0};
/* parity_idx → mask pointer (idx=0 = all-ones = implicit) */

/* ── parity descriptor ───────────────────────────
 * 3 per block, parity_idx = 0/1/2                */
typedef struct {
    uint8_t  data[TSTREAM_DATA_BYTES];
    uint16_t chunk_sizes[FEC_CHUNKS_PER_BLOCK];  /* exact per-chunk size */
    uint8_t  fec_type;     /* FEC_TYPE_XOR        */
    uint8_t  fec_n;        /* 3 (per block)       */
    uint8_t  level;        /* 0-4                 */
    uint8_t  block;        /* 0-11                */
    uint8_t  parity_idx;   /* 0=all 1=S3A 2=S3B  */
    uint8_t  _pad[3];
} FECParity;

/* ── internal: XOR one block with optional mask ─ */
static inline void _fec_xor_block(
    const TStreamChunk store[FEC_TOTAL_DATA],
    uint16_t base, const uint8_t *mask,   /* mask=NULL → include all */
    uint8_t out_data[TSTREAM_DATA_BYTES],
    uint16_t out_sizes[FEC_CHUNKS_PER_BLOCK])
{
    memset(out_data, 0, TSTREAM_DATA_BYTES);
    for (uint8_t i = 0u; i < FEC_CHUNKS_PER_BLOCK; i++) {
        if (mask && !mask[i]) continue;
        uint16_t pos = (uint16_t)(base + i);
        uint16_t sz  = store[pos].size;
        if (out_sizes) out_sizes[i] = sz;
        for (uint16_t b = 0u; b < sz; b++)
            out_data[b] ^= store[pos].data[b];
    }
}

/* ── encode: fill 3 parity slots for one block ── */
static inline void fec_encode_block(
    const TStreamChunk store[FEC_TOTAL_DATA],
    uint8_t level, uint8_t block,
    FECParity out[FEC_PARITY_PER_BLOCK])   /* out[0..2] */
{
    uint16_t base = (uint16_t)(level * GEO_PYR_PHASE_LEN
                              + block * FEC_CHUNKS_PER_BLOCK);

    /* P0: all chunks */
    _fec_xor_block(store, base, NULL, out[0].data, out[0].chunk_sizes);
    /* P1: stride-3 phase A */
    _fec_xor_block(store, base, FEC_S3A, out[1].data, NULL);
    memcpy(out[1].chunk_sizes, out[0].chunk_sizes, sizeof(out[0].chunk_sizes));
    /* P2: stride-3 phase B */
    _fec_xor_block(store, base, FEC_S3B, out[2].data, NULL);
    memcpy(out[2].chunk_sizes, out[0].chunk_sizes, sizeof(out[0].chunk_sizes));

    for (uint8_t k = 0u; k < FEC_PARITY_PER_BLOCK; k++) {
        out[k].fec_type   = FEC_TYPE_XOR;
        out[k].fec_n      = FEC_PARITY_PER_BLOCK;
        out[k].level      = level;
        out[k].block      = block;
        out[k].parity_idx = k;
    }
}

/* ── encode all: fill parity[180] ─────────────── */
static inline void fec_encode_all(
    const TStreamChunk store[FEC_TOTAL_DATA],
    FECParity           parity[FEC_TOTAL_PARITY])
{
    for (uint8_t l = 0u; l < FEC_LEVELS; l++)
        for (uint8_t b = 0u; b < FEC_BLOCKS_PER_LEVEL; b++) {
            uint8_t idx = (uint8_t)(l * FEC_BLOCKS_PER_LEVEL + b);
            fec_encode_block(store, l, b,
                             &parity[idx * FEC_PARITY_PER_BLOCK]);
        }
}

/* ── recover one block given its 3 parity structs
 * par[0]=P0(all), par[1]=P1(S3A), par[2]=P2(S3B)
 * Returns number of chunks recovered (0, 1, or 2)
 * Fills store + marks ring.present for recovered slots  */
static inline uint8_t fec_recover_block(
    TRingCtx          *r,
    TStreamChunk       store[FEC_TOTAL_DATA],
    const FECParity    par[FEC_PARITY_PER_BLOCK])
{
    uint16_t base = (uint16_t)(par[0].level * GEO_PYR_PHASE_LEN
                               + par[0].block * FEC_CHUNKS_PER_BLOCK);

    /* find missing slots */
    uint8_t miss[FEC_CHUNKS_PER_BLOCK];
    uint8_t nm = 0u;
    for (uint8_t i = 0u; i < FEC_CHUNKS_PER_BLOCK; i++) {
        uint16_t pos = (uint16_t)(base + i);
        if (!r->slots[pos].present) miss[nm++] = i;
    }
    if (nm == 0u) return 0u;

    /* ── case 1: single loss → P0 recovers directly ── */
    if (nm == 1u) {
        uint8_t  mi  = miss[0];
        uint16_t pos = (uint16_t)(base + mi);

        uint8_t recovered[TSTREAM_DATA_BYTES];
        memcpy(recovered, par[0].data, TSTREAM_DATA_BYTES);
        for (uint8_t i = 0u; i < FEC_CHUNKS_PER_BLOCK; i++) {
            if (i == mi) continue;
            uint16_t sz = store[base + i].size;
            for (uint16_t b = 0u; b < sz; b++)
                recovered[b] ^= store[base + i].data[b];
        }
        uint16_t sz = par[0].chunk_sizes[mi];
        memcpy(store[pos].data, recovered, sz);
        memset(store[pos].data + sz, 0, TSTREAM_DATA_BYTES - sz);
        store[pos].size         = sz;
        r->slots[pos].present   = 1u;
        return 1u;
    }

    /* ── case 2: two losses → try all 3 parity pairs ─
     *
     * For missing indices (mi, mj) and parity Pk covering mask M:
     *   Pk_residual = Pk.data XOR XOR(present chunks in M)
     *              = M[mi]*C[mi] XOR M[mj]*C[mj]
     *
     * If exactly one of M[mi], M[mj] is 1 → that chunk is known directly.
     * Then use any other parity to get the second.
     */
    if (nm == 2u) {
        uint8_t mi = miss[0], mj = miss[1];

        /* masks: P0=all(1,1), P1=S3A, P2=S3B */
        const uint8_t *masks[3] = {NULL, FEC_S3A, FEC_S3B};
        /* P0 mask treated as all-ones */

        /* compute residuals: Pk XOR present */
        uint8_t res[3][TSTREAM_DATA_BYTES];
        for (uint8_t k = 0u; k < 3u; k++) {
            memcpy(res[k], par[k].data, TSTREAM_DATA_BYTES);
            for (uint8_t i = 0u; i < FEC_CHUNKS_PER_BLOCK; i++) {
                if (i == mi || i == mj) continue;
                uint8_t in_mask = masks[k] ? masks[k][i] : 1u;
                if (!in_mask) continue;
                uint16_t sz = store[base + i].size;
                for (uint16_t b = 0u; b < sz; b++)
                    res[k][b] ^= store[base + i].data[b];
            }
        }

        /* try each parity k: if mi and mj differ in mask → isolate one */
        for (uint8_t k = 0u; k < 3u; k++) {
            uint8_t cmi = masks[k] ? masks[k][mi] : 1u;
            uint8_t cmj = masks[k] ? masks[k][mj] : 1u;

            /* need exactly one bit set: cmi XOR cmj = 1 */
            if (cmi == cmj) continue;

            /* which index is the isolated one in Pk? */
            uint8_t iso  = cmi ? mi : mj;   /* the one covered */
            uint8_t other = (iso == mi) ? mj : mi;

            /* res[k] = C[iso] (since only iso is in mask) */
            uint16_t sz_iso = par[k].chunk_sizes[iso];
            uint16_t pos_iso   = (uint16_t)(base + iso);
            uint16_t pos_other = (uint16_t)(base + other);

            memcpy(store[pos_iso].data, res[k], sz_iso);
            memset(store[pos_iso].data + sz_iso, 0, TSTREAM_DATA_BYTES - sz_iso);
            store[pos_iso].size       = sz_iso;
            r->slots[pos_iso].present = 1u;

            /* now nm=1: use P0 residual to get 'other' */
            /* re-XOR P0 residual with newly recovered iso */
            uint8_t rec2[TSTREAM_DATA_BYTES];
            memcpy(rec2, res[0], TSTREAM_DATA_BYTES);       /* P0 residual */
            for (uint16_t b = 0u; b < sz_iso; b++)
                rec2[b] ^= store[pos_iso].data[b];

            uint16_t sz_other = par[0].chunk_sizes[other];
            memcpy(store[pos_other].data, rec2, sz_other);
            memset(store[pos_other].data + sz_other, 0, TSTREAM_DATA_BYTES - sz_other);
            store[pos_other].size        = sz_other;
            r->slots[pos_other].present  = 1u;
            return 2u;
        }
        return 0u;  /* unrecoverable: all parity equations dependent for (mi,mj) */
    }

    return 0u;  /* nm >= 3: beyond 2-parity scope */
}

/* ── recover all 60 blocks ───────────────────────
 * parity layout: parity[block_idx * 3 + 0/1/2]
 * Returns total chunks recovered                 */
static inline uint16_t fec_recover_all(
    TRingCtx        *r,
    TStreamChunk     store[FEC_TOTAL_DATA],
    const FECParity  parity[FEC_TOTAL_PARITY])
{
    uint16_t total = 0u;
    for (uint8_t l = 0u; l < FEC_LEVELS; l++)
        for (uint8_t b = 0u; b < FEC_BLOCKS_PER_LEVEL; b++) {
            uint8_t idx = (uint8_t)(l * FEC_BLOCKS_PER_LEVEL + b);
            total += fec_recover_block(r, store,
                                       &parity[idx * FEC_PARITY_PER_BLOCK]);
        }
    return total;
}

/* ── gap map ─────────────────────────────────────*/
static inline uint16_t fec_gap_map(
    const TRingCtx *r,
    uint16_t        n_pkts,
    uint8_t         gap_map[FEC_TOTAL_DATA])
{
    uint16_t gaps = 0u;
    for (uint16_t i = 0u; i < n_pkts; i++) {
        gap_map[i] = r->slots[i].present ? 0u : 1u;
        if (!r->slots[i].present) gaps++;
    }
    return gaps;
}

#endif /* GEO_FEC_H */
