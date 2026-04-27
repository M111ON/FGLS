 * tring_fec_recv — RECEIVER: ingest one packet
 *   Wraps tstream_recv_pkt. Returns gap count from ring.
 * ══════════════════════════════════════════════════ */
static inline int tring_fec_recv(
    TRingFECCtx     *ctx,
    const TStreamPkt *pkt)
{
    return tstream_recv_pkt(&ctx->ring, ctx->store, pkt);
}

/* ══════════════════════════════════════════════════
 * tring_fec_recover — RECEIVER: RS gap recovery
 *   Call after all packets received (or timeout).
 *   Requires ctx.parity_pool to be loaded (from sender).
 *   Returns chunks recovered.
 * ══════════════════════════════════════════════════ */
static inline uint16_t tring_fec_recover(TRingFECCtx *ctx) {
    uint8_t gmap[FEC_TOTAL_DATA];
    ctx->gaps_before = fec_gap_map(&ctx->ring, ctx->n_pkts, gmap);

    if (ctx->gaps_before == 0u) {
        ctx->recovered  = 0u;
        ctx->gaps_after = 0u;
        return 0u;
    }

    ctx->recovered  = fec_rs_recover_all(&ctx->ring, ctx->store,
                                          ctx->fec_n, ctx->parity_pool);
    ctx->gaps_after = fec_gap_map(&ctx->ring, ctx->n_pkts, gmap);
    return ctx->recovered;
}

/* ══════════════════════════════════════════════════
 * tring_fec_reconstruct — RECEIVER: write output buffer
 *   Returns bytes written.
 * ══════════════════════════════════════════════════ */
static inline uint32_t tring_fec_reconstruct(
    const TRingFECCtx *ctx,
    uint8_t           *out,
    uint32_t           out_max)
{
    (void)out_max;
    return tstream_reconstruct(&ctx->ring, ctx->store, ctx->n_pkts, out);
}

/* ══════════════════════════════════════════════════
 * tring_fec_reconstruct_exact — skip gap zero-fill