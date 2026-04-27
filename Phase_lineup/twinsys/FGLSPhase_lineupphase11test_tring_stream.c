#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "geo_temporal_lut.h"
#include "geo_temporal_ring.h"
#include "geo_pyramid.h"
#include "geo_tring_stream.h"

static int pass=0, fail=0;
#define ASSERT(c,m) do{if(c){printf("  ✓ %s\n",m);pass++;}else{printf("  ✗ FAIL: %s\n",m);fail++;}}while(0)

/* ── test file buffers ─────────────────────────────────────────── */
static TStreamPkt   pkts[TSTREAM_MAX_PKTS];
static TStreamChunk store[TSTREAM_MAX_PKTS];
static uint8_t      out_buf[TSTREAM_MAX_PKTS * TSTREAM_DATA_BYTES];

int main(void){
    printf("╔══════════════════════════════════╗\n");
    printf("║  Phase 8 — TRing Stream Tests    ║\n");
    printf("╚══════════════════════════════════╝\n");

    /* T1: packet size invariant */
    printf("\n[T1] packet layout\n");
    ASSERT(sizeof(TStreamPkt) == TSTREAM_PKT_BYTES, "TStreamPkt = 4104B");
    ASSERT(TSTREAM_PKT_BYTES  == 4104u,             "4104 = enc(4)+size(2)+crc(2)+data(4096)");

    /* T2: slice tiny file (fits in 1 chunk) */
    printf("\n[T2] slice tiny file (1 chunk)\n");
    {
        uint8_t file[16] = "hello_tring_geo!";
        uint16_t n = tstream_slice_file(pkts, file, 16u);
        ASSERT(n == 1,                    "1 packet for 16B file");
        ASSERT(pkts[0].enc == GEO_WALK[0], "chunk 0 enc = GEO_WALK[0]");
        ASSERT(pkts[0].size == 16u,        "chunk size = 16");
        ASSERT(memcmp(pkts[0].data, file, 16u)==0, "data correct");
        /* crc check */
        uint16_t crc = _tstream_crc16(pkts[0].data, pkts[0].size);
        ASSERT(crc == pkts[0].crc16, "CRC16 correct");
    }

    /* T3: slice file spanning 3 chunks */
    printf("\n[T3] slice multi-chunk file\n");
    {
        /* 2×4096 + 100 = 8292B → 3 chunks */
        static uint8_t file3[8292];
        for(int i=0;i<8292;i++) file3[i]=(uint8_t)(i & 0xFF);
        uint16_t n = tstream_slice_file(pkts, file3, 8292u);
        ASSERT(n == 3, "3 packets for 8292B file");
        ASSERT(pkts[0].enc == GEO_WALK[0], "chunk 0 enc = GEO_WALK[0]");
        ASSERT(pkts[1].enc == GEO_WALK[1], "chunk 1 enc = GEO_WALK[1]");
        ASSERT(pkts[2].enc == GEO_WALK[2], "chunk 2 enc = GEO_WALK[2]");
        ASSERT(pkts[0].size == 4096u,      "chunk 0 size = 4096");
        ASSERT(pkts[1].size == 4096u,      "chunk 1 size = 4096");
        ASSERT(pkts[2].size == 100u,       "chunk 2 size = 100 (remainder)");
        /* verify CRCs */
        int crc_ok=1;
        for(int i=0;i<3;i++){
            uint16_t crc = _tstream_crc16(pkts[i].data, pkts[i].size);
            if(crc != pkts[i].crc16){ crc_ok=0; break; }
        }
        ASSERT(crc_ok, "all 3 CRCs correct");
    }

    /* T4: recv in order → gap=0 */
    printf("\n[T4] recv in-order, no gap\n");
    {
        TRingCtx r; tring_init(&r);
        memset(store, 0, sizeof(store));
        static uint8_t file4[8292];
        for(int i=0;i<8292;i++) file4[i]=(uint8_t)(i & 0xFF);
        uint16_t n = tstream_slice_file(pkts, file4, 8292u);
        /* recv chunk 0 first: head starts at 0, snap to 0 */
        int g0 = tstream_recv_pkt(&r, store, &pkts[0]);
        /* head starts 0, snap to pos 0 → gap=0 */
        ASSERT(g0 == 0, "recv chunk 0: gap=0");
        int g1 = tstream_recv_pkt(&r, store, &pkts[1]);
        /* head=0 after chunk0, snap to pos 1 → gap=1 (one step forward) */
        ASSERT(g1 == 1, "recv chunk 1: gap=1 (head 0→1)");
        int g2 = tstream_recv_pkt(&r, store, &pkts[2]);
        /* head=1, snap to pos 2 → gap=1 */
        ASSERT(g2 == 1, "recv chunk 2: gap=1 (head 1→2)");
        ASSERT(r.slots[0].present, "slot 0 present");
        ASSERT(r.slots[1].present, "slot 1 present");
        ASSERT(r.slots[2].present, "slot 2 present");
    }

    /* T5: recv out-of-order → gap detected, geometry heals */
    printf("\n[T5] recv out-of-order (gap healing)\n");
    {
        TRingCtx r; tring_init(&r);
        memset(store, 0, sizeof(store));
        static uint8_t file5[8292];
        for(int i=0;i<8292;i++) file5[i]=(uint8_t)(i*3 & 0xFF);
        tstream_slice_file(pkts, file5, 8292u);
        /* recv chunk 2 first (skip 0,1) */
        int g = tstream_recv_pkt(&r, store, &pkts[2]);
        ASSERT(g == 2, "recv chunk 2 first: gap=2 (snapped over 0,1)");
        ASSERT(r.head == 2,   "head snapped to 2");
        ASSERT(r.missing == 2, "missing=2 tracked");
    }

    /* T6: CRC fail → dropped, -2 returned */
    printf("\n[T6] CRC fail detection\n");
    {
        TRingCtx r; tring_init(&r);
        memset(store, 0, sizeof(store));
        uint8_t file6[100]; memset(file6, 0xAB, 100);
        tstream_slice_file(pkts, file6, 100u);
        /* corrupt data */
        pkts[0].data[0] ^= 0xFF;
        int res = tstream_recv_pkt(&r, store, &pkts[0]);
        ASSERT(res == -2, "corrupted pkt returns -2");
        ASSERT(!r.slots[0].present, "corrupt pkt not stored");
    }

    /* T7: unknown enc → -1 */
    printf("\n[T7] unknown enc\n");
    {
        TRingCtx r; tring_init(&r);
        memset(store, 0, sizeof(store));
        TStreamPkt bad; memset(&bad, 0, sizeof(bad));
        bad.enc = 0xFFFFFFFFu;   /* not in GEO_WALK */
        int res = tstream_recv_pkt(&r, store, &bad);
        ASSERT(res == -1, "unknown enc returns -1");
    }

    /* T8: full round-trip — slice → recv → reconstruct */
    printf("\n[T8] full round-trip reconstruct\n");
    {
        TRingCtx r; tring_init(&r);
        memset(store, 0, sizeof(store));
        memset(out_buf, 0, sizeof(out_buf));

        /* build a known file */
        static uint8_t src[9000];
        for(int i=0;i<9000;i++) src[i]=(uint8_t)((i*7+13) & 0xFF);

        uint16_t n = tstream_slice_file(pkts, src, 9000u);
        ASSERT(n == 3, "9000B sliced into 3 chunks");

        /* recv all in order */
        for(uint16_t i=0;i<n;i++) tstream_recv_pkt(&r, store, &pkts[i]);

        /* reconstruct */
        uint32_t bytes = tstream_reconstruct(&r, store, n, out_buf);
        /* last chunk is 9000 - 2×4096 = 808B; total = 2×4096 + 808 = 9000 */
        ASSERT(bytes == 9000u, "reconstructed exactly 9000B");
        ASSERT(memcmp(out_buf, src, 9000u)==0, "reconstructed data matches source");
    }

    /* T9: reconstruct_exact skips gaps */
    printf("\n[T9] reconstruct_exact (compact, skip gaps)\n");
    {
        TRingCtx r; tring_init(&r);
        memset(store, 0, sizeof(store));
        memset(out_buf, 0, sizeof(out_buf));

        static uint8_t src2[8292];
        for(int i=0;i<8292;i++) src2[i]=(uint8_t)(i & 0xFF);
        uint16_t n = tstream_slice_file(pkts, src2, 8292u);

        /* only recv chunk 0 and 2, skip 1 */
        tstream_recv_pkt(&r, store, &pkts[0]);
        tstream_recv_pkt(&r, store, &pkts[2]);

        uint32_t bytes = tstream_reconstruct_exact(&r, store, n, out_buf);
        /* chunk0=4096 + chunk2=100 = 4196 */
        ASSERT(bytes == 4196u, "reconstruct_exact: 4096+100=4196B (gap skipped)");
        ASSERT(memcmp(out_buf, src2, 4096u)==0, "chunk 0 data correct");
        ASSERT(memcmp(out_buf+4096u, pkts[2].data, 100u)==0, "chunk 2 data correct");
    }

    /* T10: tstream_gap_report */
    printf("\n[T10] gap_report\n");
    {
        TRingCtx r; tring_init(&r);
        memset(store, 0, sizeof(store));
        static uint8_t src3[5*4096];
        memset(src3, 0x55, sizeof(src3));
        uint16_t n = tstream_slice_file(pkts, src3, sizeof(src3));
        ASSERT(n == 5, "5 chunks");
        /* recv 0,2,4 — skip 1,3 */
        tstream_recv_pkt(&r, store, &pkts[0]);
        tstream_recv_pkt(&r, store, &pkts[2]);
        tstream_recv_pkt(&r, store, &pkts[4]);
        uint16_t gaps = tstream_gap_report(&r, n);
        ASSERT(gaps == 2u, "gap_report=2 (slots 1,3 missing)");
    }

    /* T11: tstream_phase_ready — level complete check via pyramid */
    printf("\n[T11] phase_ready bridges pyramid + stream\n");
    {
        TRingCtx r; tring_init(&r);
        memset(store, 0, sizeof(store));
        ASSERT(tstream_phase_ready(&r, 0)==0, "L0 not ready initially");
        /* fill all 144 slots of L0 */
        static uint8_t dummy[144 * 4096];
        memset(dummy, 0x11, sizeof(dummy));
        uint16_t n = tstream_slice_file(pkts, dummy, sizeof(dummy));
        ASSERT(n == 144u, "144 chunks for L0");
        for(uint16_t i=0;i<144u;i++) tstream_recv_pkt(&r, store, &pkts[i]);
        ASSERT(tstream_phase_ready(&r, 0)==1, "L0 ready after all 144 recv");
        ASSERT(tstream_phase_ready(&r, 1)==0, "L1 still not ready");
    }

    /* T12: too-large file → slice returns 0 */
    printf("\n[T12] file too large (>720 chunks)\n");
    {
        /* 721 chunks = 721×4096 = 2,953,216B — just over limit */
        static uint8_t big[721 * 4096];
        memset(big, 0, sizeof(big));
        uint16_t n = tstream_slice_file(pkts, big, sizeof(big));
        ASSERT(n == 0u, "file > 720 chunks → slice returns 0");
    }

    /* T13: empty file → 0 packets */
    printf("\n[T13] empty file\n");
    {
        uint8_t empty[1] = {0};
        uint16_t n = tstream_slice_file(pkts, empty, 0u);
        ASSERT(n == 0u, "fsize=0 → 0 packets");
    }

    printf("\n══════════════════════════════════\n");
    printf("Result: %d/%d passed%s\n", pass, pass+fail, fail==0?"  ✅ ALL PASS":"  ❌");
    return fail?1:0;
}
