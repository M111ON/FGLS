/*
 * geo_tring_stream.h — TRing File Streaming Layer (Phase 8)
 * ══════════════════════════════════════════════════════════
 *
 * Wires chunk_id ↔ walk position via TRing geometry.
 *
 * Packet format (4104B total):
 *   [enc:4B][size:2B][crc16:2B][data:4096B]
 *
 * Sender:  slice file → N packets, each carrying enc = GEO_WALK[chunk_id]
 * Receiver: recv_pkt → mark slot present, verify CRC
 * Reconstruct: walk order → output buffer, gap → zero-filled
 *
 * chunk_id = walk position (0..719) — geometry IS the order.
 * No reorder buffer. No sort. No timestamp.
 *
 * No malloc. No float. No heap.
 * ══════════════════════════════════════════════════════════
 */

#ifndef GEO_TRING_STREAM_H
#define GEO_TRING_STREAM_H

#include <stdint.h>
#include <string.h>
#include "geo_temporal_ring.h"
#include "geo_pyramid.h"

/* ── Constants ────────────────────────────────────────────────── */
#define TSTREAM_DATA_BYTES   4096u
#define TSTREAM_PKT_BYTES    4104u   /* enc(4) + size(2) + crc16(2) + data(4096) */
#define TSTREAM_MAX_PKTS     TEMPORAL_WALK_LEN   /* 720 packets max per file */

typedef char _tstream_pkt_assert[(TSTREAM_PKT_BYTES == 4104u) ? 1:-1];

/* ── Packet ───────────────────────────────────────────────────── */
typedef struct {
    uint32_t enc;                     /* GEO_WALK[chunk_id] — position = order */
    uint16_t size;                    /* actual data bytes in this chunk        */
    uint16_t crc16;                   /* CRC16 over data[0..size-1]             */
    uint8_t  data[TSTREAM_DATA_BYTES];
} TStreamPkt;

typedef char _tstream_pkt_sz[(sizeof(TStreamPkt) == TSTREAM_PKT_BYTES) ? 1:-1];

/* ── Chunk store: parallel to TRing slots ─────────────────────── */
typedef struct {
    uint8_t  data[TSTREAM_DATA_BYTES];
    uint16_t size;
    uint8_t  _pad[6];
} TStreamChunk;   /* 4104B — cache-aligned per slot */

/* ── CRC16-CCITT (no table, portable) ─────────────────────────── */
static inline uint16_t _tstream_crc16(const uint8_t *buf, uint16_t len)
{
    uint16_t crc = 0xFFFFu;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)buf[i] << 8);
        for (uint8_t b = 0; b < 8u; b++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
    }
    return crc;
}

/* ── tstream_slice_file: file → TStream packets ──────────────────
 * Slices file into chunks, assigns enc = GEO_WALK[chunk_id].
 * Returns number of packets written (0 if file too large for 720 chunks).
 * out_pkts must hold TSTREAM_MAX_PKTS entries.                   */
static inline uint16_t tstream_slice_file(TStreamPkt       out_pkts[TSTREAM_MAX_PKTS],
                                           const uint8_t   *file,
                                           uint32_t         fsize)
{
    if (fsize == 0u) return 0u;

    /* total chunks needed */
    uint16_t n = (uint16_t)((fsize + TSTREAM_DATA_BYTES - 1u) / TSTREAM_DATA_BYTES);
    if (n > TSTREAM_MAX_PKTS) return 0u;   /* file too large */

    for (uint16_t i = 0u; i < n; i++) {
        TStreamPkt *p = &out_pkts[i];
        uint32_t off  = (uint32_t)i * TSTREAM_DATA_BYTES;
        uint16_t sz   = (uint16_t)((off + TSTREAM_DATA_BYTES <= fsize)
                                    ? TSTREAM_DATA_BYTES
                                    : fsize - off);

        p->enc  = GEO_WALK[i];           /* chunk_id = walk pos i         */
        p->size = sz;
        memset(p->data, 0, TSTREAM_DATA_BYTES);
        memcpy(p->data, file + off, sz);
        p->crc16 = _tstream_crc16(p->data, sz);
    }
    return n;
}

/* ── tstream_recv_pkt: receive one packet into ring + store ──────
 * Returns:  0 = ok, on-path
 *          >0 = gap count (snapped ahead, geometry-healed)
 *          -1 = unknown enc
 *          -2 = CRC fail (dropped)                               */
static inline int tstream_recv_pkt(TRingCtx        *r,
                                    TStreamChunk    store[TSTREAM_MAX_PKTS],
                                    const TStreamPkt *pkt)
{
    /* decode walk position */
    uint16_t pos = tring_pos(pkt->enc);
    if (pos == 0xFFFFu) return -1;

    /* CRC check */
    uint16_t crc = _tstream_crc16(pkt->data, pkt->size);
    if (crc != pkt->crc16) return -2;

    /* store chunk at walk position */
    memcpy(store[pos].data, pkt->data, pkt->size);
    store[pos].size = pkt->size;

    /* snap ring — returns gap count */
    return tring_snap(r, pkt->enc);
}

/* ── tstream_reconstruct: walk order → output buffer ────────────
 * Writes chunks in walk order (0..n_pkts-1) into out.
 * Gaps (missing slots) are zero-filled.
 * n_pkts: total expected packets (from sender's slice count).
 * Returns total bytes written (including zero-filled gaps).      */
static inline uint32_t tstream_reconstruct(const TRingCtx      *r,
                                            const TStreamChunk   store[TSTREAM_MAX_PKTS],
                                            uint16_t             n_pkts,
                                            uint8_t             *out)
{
    uint32_t written = 0u;
    for (uint16_t i = 0u; i < n_pkts; i++) {
        if (r->slots[i].present) {
            uint16_t sz = store[i].size;
            memcpy(out + written, store[i].data, sz);
            written += sz;
        } else {
            /* gap: zero-fill a full chunk slot */
            memset(out + written, 0, TSTREAM_DATA_BYTES);
            written += TSTREAM_DATA_BYTES;
        }
    }
    return written;
}

/* ── tstream_reconstruct_exact: skip gap zero-fill ───────────────
 * Only writes present chunks — output is compact, no gap padding.
 * Use when gaps are expected and caller handles missing chunks.
 * Returns bytes written (may be less than full file if gaps exist). */
static inline uint32_t tstream_reconstruct_exact(const TRingCtx      *r,
                                                   const TStreamChunk   store[TSTREAM_MAX_PKTS],
                                                   uint16_t             n_pkts,
                                                   uint8_t             *out)
{
    uint32_t written = 0u;
    for (uint16_t i = 0u; i < n_pkts; i++) {
        if (!r->slots[i].present) continue;
        uint16_t sz = store[i].size;
        memcpy(out + written, store[i].data, sz);
        written += sz;
    }
    return written;
}

/* ── tstream_phase_ready: check if a pyramid level is complete ───
 * Returns 1 if all slots in level are present — safe to flush/dispatch.
 * Bridges geo_pyramid.h into streaming decision point.           */
static inline int tstream_phase_ready(const TRingCtx *r, uint8_t level)
{
    PyramidPhase ph = pyr_scan_phase(r, level);
    return ph.complete;
}

/* ── tstream_gap_report: count gaps in first n_pkts slots ────────
 * O(n_pkts)                                                       */
static inline uint16_t tstream_gap_report(const TRingCtx *r, uint16_t n_pkts)
{
    uint16_t gaps = 0u;
    for (uint16_t i = 0u; i < n_pkts; i++)
        if (!r->slots[i].present) gaps++;
    return gaps;
}

/* ════════════════════════════════════════════════════════════════
 * SEGMENTED STREAMING — files larger than 720×4096 = 2,949,120B
 * ════════════════════════════════════════════════════════════════
 *
 * Splits file into segments, each ≤ 720 chunks (≤ 2.81MB).
 * Each segment runs through one full TRing pass.
 * TRing is reset between segments — sacred numbers unchanged.
 *
 * Packet format extended (4112B):
 *   [enc:4B][size:2B][crc16:2B][seg_id:2B][seg_total:2B][data:4096B]
 *
 * seg_id    = 0-based segment index
 * seg_total = total number of segments for this file
 * CRC covers data[0..size-1] only (same as before)
 * ════════════════════════════════════════════════════════════════ */

#define TSTREAM_SEG_PKT_BYTES  4108u  /* enc(4)+size(2)+crc16(2)+seg_id(2)+seg_total(2)+data(4096) */
#define TSTREAM_MAX_SEGS       65535u /* uint16 ceiling, practical limit much lower */

/* max file size = 720 segments × 720 chunks × 4096B = ~2.1GB */

typedef struct {
    uint32_t enc;                      /* GEO_WALK[chunk_id]              */
    uint16_t size;                     /* actual data bytes                */
    uint16_t crc16;                    /* CRC16 over data[0..size-1]       */
    uint16_t seg_id;                   /* which segment (0-based)          */
    uint16_t seg_total;                /* total segments for this file     */
    uint8_t  data[TSTREAM_DATA_BYTES];
} TStreamPktSeg;

typedef char _tstream_seg_pkt_sz[(sizeof(TStreamPktSeg) == TSTREAM_SEG_PKT_BYTES) ? 1:-1];

/* ── tstream_seg_count: how many segments a file needs ───────────
 * O(1)                                                            */
static inline uint32_t tstream_seg_count(uint32_t fsize)
{
    if (fsize == 0u) return 0u;
    uint32_t chunks = (fsize + TSTREAM_DATA_BYTES - 1u) / TSTREAM_DATA_BYTES;
    return (chunks + TSTREAM_MAX_PKTS - 1u) / TSTREAM_MAX_PKTS;
}

/* ── tstream_slice_seg: slice one segment into TStreamPktSeg[] ───
 * seg_id: which segment to slice (0-based)
 * out_pkts: must hold TSTREAM_MAX_PKTS entries
 * Returns number of packets in this segment (0 on error).        */
static inline uint16_t tstream_slice_seg(TStreamPktSeg    out_pkts[TSTREAM_MAX_PKTS],
                                          const uint8_t   *file,
                                          uint32_t         fsize,
                                          uint16_t         seg_id,
                                          uint16_t         seg_total)
{
    uint32_t seg_off  = (uint32_t)seg_id * TSTREAM_MAX_PKTS * TSTREAM_DATA_BYTES;
    if (seg_off >= fsize) return 0u;

    uint32_t seg_size = fsize - seg_off;
    if (seg_size > TSTREAM_MAX_PKTS * TSTREAM_DATA_BYTES)
        seg_size = TSTREAM_MAX_PKTS * TSTREAM_DATA_BYTES;

    uint16_t n = (uint16_t)((seg_size + TSTREAM_DATA_BYTES - 1u) / TSTREAM_DATA_BYTES);

    for (uint16_t i = 0u; i < n; i++) {
        TStreamPktSeg *p = &out_pkts[i];
        uint32_t off = seg_off + (uint32_t)i * TSTREAM_DATA_BYTES;
        uint16_t sz  = (uint16_t)((off + TSTREAM_DATA_BYTES <= fsize)
                                   ? TSTREAM_DATA_BYTES
                                   : fsize - off);
        p->enc       = GEO_WALK[i];
        p->size      = sz;
        p->seg_id    = seg_id;
        p->seg_total = seg_total;
        memset(p->data, 0, TSTREAM_DATA_BYTES);
        memcpy(p->data, file + off, sz);
        p->crc16 = _tstream_crc16(p->data, sz);
    }
    return n;
}

/* ── tstream_recv_seg: receive one TStreamPktSeg ─────────────────
 * r must be reset (tring_init) at the start of each segment.
 * Returns: 0/gap = ok, -1 = unknown enc, -2 = CRC fail          */
static inline int tstream_recv_seg(TRingCtx          *r,
                                    TStreamChunk       store[TSTREAM_MAX_PKTS],
                                    const TStreamPktSeg *pkt)
{
    uint16_t pos = tring_pos(pkt->enc);
    if (pos == 0xFFFFu) return -1;

    uint16_t crc = _tstream_crc16(pkt->data, pkt->size);
    if (crc != pkt->crc16) return -2;

    memcpy(store[pos].data, pkt->data, pkt->size);
    store[pos].size = pkt->size;
    return tring_snap(r, pkt->enc);
}

/* ── tstream_reconstruct_segmented: multi-segment → output buf ───
 * Caller passes array of (r, store, n_pkts) per segment.
 * Writes all segments contiguously into out.
 * Returns total bytes written.
 *
 * Usage pattern:
 *   for each seg_id 0..seg_total-1:
 *     tring_init(&r[seg_id])
 *     recv all pkts for that seg → tstream_recv_seg(&r[seg_id], store[seg_id], pkt)
 *   tstream_reconstruct_segmented(r, store, n_per_seg, seg_total, out)
 *
 * n_per_seg[i] = number of chunks in segment i                   */
static inline uint32_t tstream_reconstruct_segmented(
    const TRingCtx    *r_arr,
    const TStreamChunk store_arr[][TSTREAM_MAX_PKTS],
    const uint16_t    *n_per_seg,
    uint16_t           seg_total,
    uint8_t           *out)
{
    uint32_t written = 0u;
    for (uint16_t s = 0u; s < seg_total; s++) {
        written += tstream_reconstruct(&r_arr[s],
                                        store_arr[s],
                                        n_per_seg[s],
                                        out + written);
    }
    return written;
}

#endif /* GEO_TRING_STREAM_H */
