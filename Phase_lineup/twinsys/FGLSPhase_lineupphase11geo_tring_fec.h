/*
 * geo_tring_fec.h — TRing + RS-FEC integration helpers
 * ====================================================
 *
 * A thin operational wrapper over:
 *
 * - geo_temporal_ring.h
 * - geo_tring_stream.h
 * - geo_fec_rs.h
 *
 * This file provides a sender/receiver context used by
 * test_tring_fec.c and by higher-level service wrappers.
 */

#ifndef GEO_TRING_FEC_H
#define GEO_TRING_FEC_H

#include <stdint.h>
#include <string.h>

#include "geo_temporal_ring.h"
#include "geo_tring_stream.h"
#include "geo_fec.h"
#include "geo_fec_rs.h"
#include "geo_rewind.h"

typedef struct {
    TRingCtx      ring;
    TStreamChunk  store[FEC_TOTAL_DATA];
    FECParity     parity_pool[FEC_LEVELS * FEC_BLOCKS_PER_LEVEL * FEC_CHUNKS_PER_BLOCK];
    RewindBuffer  rewind;
    uint16_t      n_pkts;
    uint16_t      gaps_before;
    uint16_t      recovered;
    uint16_t      gaps_after;
    uint8_t       fec_n;
} TRingFECCtx;

typedef struct {
    uint16_t n_pkts;
    uint16_t gaps_before;
    uint16_t recovered;
    uint16_t gaps_after;
    uint8_t  fec_n;
    uint8_t  loss_pct;
} TRingFECStats;

static inline void tring_fec_prepare(TRingFECCtx *ctx, uint8_t fec_n)
{
    memset(ctx, 0, sizeof(*ctx));
    tring_init(&ctx->ring);
    rewind_init(&ctx->rewind);
    ctx->fec_n = fec_n;
}

static inline uint16_t tring_fec_encode(TRingFECCtx     *ctx,
                                        const uint8_t   *file,
                                        uint32_t         fsize)
{
    TStreamPkt pkts[TSTREAM_MAX_PKTS];
    uint16_t n = tstream_slice_file(pkts, file, fsize);
    ctx->n_pkts = n;

    for (uint16_t i = 0u; i < n; i++) {
        memcpy(ctx->store[i].data, pkts[i].data, TSTREAM_DATA_BYTES);
        ctx->store[i].size = pkts[i].size;
        ctx->ring.slots[i].present = 1u;
        ctx->ring.slots[i].enc = pkts[i].enc;
        ctx->ring.slots[i].chunk_id = i;
        ctx->ring.chunk_count++;
        rewind_store(&ctx->rewind, pkts[i].enc, &ctx->store[i]);
    }

    for (uint16_t i = n; i < FEC_TOTAL_DATA; i++) {
        memset(ctx->store[i].data, 0, TSTREAM_DATA_BYTES);
        ctx->store[i].size = 0u;
        ctx->ring.slots[i].present = 0u;
        ctx->ring.slots[i].chunk_id = i;
    }

    fec_rs_encode_all(ctx->store, ctx->fec_n, ctx->parity_pool);
    return n;
}

static inline void tring_fec_load_parity(TRingFECCtx       *dst,
                                         const TRingFECCtx *src)
{
    dst->fec_n = src->fec_n;
    memcpy(dst->parity_pool, src->parity_pool, sizeof(dst->parity_pool));
}

static inline int tring_fec_recv(TRingFECCtx      *ctx,
                                 const TStreamPkt *pkt)
{
    int rc = tstream_recv_pkt(&ctx->ring, ctx->store, pkt);
    if (rc >= 0)
        rewind_store(&ctx->rewind, pkt->enc, &ctx->store[tring_pos(pkt->enc)]);
    return rc;
}

static inline uint16_t tring_fec_recover(TRingFECCtx *ctx)
{
    uint8_t gmap[FEC_TOTAL_DATA];
    ctx->gaps_before = fec_gap_map(&ctx->ring, ctx->n_pkts, gmap);

    if (ctx->gaps_before == 0u) {
        ctx->recovered = 0u;
        ctx->gaps_after = 0u;
        return 0u;
    }

    ctx->recovered = fec_rs_recover_all(&ctx->ring, ctx->store,
                                        ctx->fec_n, ctx->parity_pool);
    ctx->gaps_after = fec_gap_map(&ctx->ring, ctx->n_pkts, gmap);
    return ctx->recovered;
}

static inline uint32_t tring_fec_reconstruct(const TRingFECCtx *ctx,
                                             uint8_t           *out,
                                             uint32_t           out_max)
{
    (void)out_max;
    return tstream_reconstruct(&ctx->ring, ctx->store, ctx->n_pkts, out);
}

static inline uint32_t tring_fec_reconstruct_exact(const TRingFECCtx *ctx,
                                                   uint8_t           *out,
                                                   uint32_t           out_max)
{
    (void)out_max;
    return tstream_reconstruct_exact(&ctx->ring, ctx->store, ctx->n_pkts, out);
}

static inline void tring_fec_stats(const TRingFECCtx *ctx, TRingFECStats *st)
{
    st->n_pkts       = ctx->n_pkts;
    st->gaps_before  = ctx->gaps_before;
    st->recovered    = ctx->recovered;
    st->gaps_after   = ctx->gaps_after;
    st->fec_n        = ctx->fec_n;
    st->loss_pct     = (ctx->n_pkts == 0u)
                     ? 0u
                     : (uint8_t)((ctx->gaps_before * 100u) / ctx->n_pkts);
}

#endif /* GEO_TRING_FEC_H */
