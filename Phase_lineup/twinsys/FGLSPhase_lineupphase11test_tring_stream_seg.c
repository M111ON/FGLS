#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "geo_temporal_lut.h"
#include "geo_temporal_ring.h"
#include "geo_pyramid.h"
#include "geo_tring_stream.h"

static int pass=0, fail=0;
#define ASSERT(c,m) do{if(c){printf("  ✓ %s\n",m);pass++;}else{printf("  ✗ FAIL: %s\n",m);fail++;}}while(0)

/* buffers — keep off stack */
#define MAX_TEST_SEGS 4
static TStreamPktSeg  seg_pkts[TSTREAM_MAX_PKTS];
static TStreamChunk   stores[MAX_TEST_SEGS][TSTREAM_MAX_PKTS];
static TRingCtx       rings[MAX_TEST_SEGS];
static uint8_t        out_buf[MAX_TEST_SEGS * TSTREAM_MAX_PKTS * TSTREAM_DATA_BYTES];

int main(void){
    printf("╔════════════════════════════════════╗\n");
    printf("║  Phase 8 — Segmented Stream Tests  ║\n");
    printf("╚════════════════════════════════════╝\n");

    /* T1: seg_count arithmetic */
    printf("\n[T1] tstream_seg_count\n");
    ASSERT(tstream_seg_count(0u)          == 0u, "0 bytes → 0 segs");
    ASSERT(tstream_seg_count(1u)          == 1u, "1 byte  → 1 seg");
    ASSERT(tstream_seg_count(4096u)       == 1u, "4096B   → 1 seg");
    ASSERT(tstream_seg_count(2949120u)    == 1u, "720×4096 = 1 full seg");
    ASSERT(tstream_seg_count(2949121u)    == 2u, "720×4096+1 → 2 segs");
    ASSERT(tstream_seg_count(2949120u*2u) == 2u, "2×full → 2 segs");
    ASSERT(tstream_seg_count(2949120u*3u+1u) == 4u, "3×full+1B → 4 segs");

    /* T2: packet size */
    printf("\n[T2] TStreamPktSeg layout\n");
    ASSERT(sizeof(TStreamPktSeg) == TSTREAM_SEG_PKT_BYTES, "TStreamPktSeg = 4112B");

    /* T3: slice single segment (file fits in 1 seg) */
    printf("\n[T3] slice_seg single segment\n");
    {
        static uint8_t file[9000];
        for(int i=0;i<9000;i++) file[i]=(uint8_t)(i & 0xFF);
        uint16_t seg_tot = (uint16_t)tstream_seg_count(9000u);
        uint16_t n = tstream_slice_seg(seg_pkts, file, 9000u, 0, seg_tot);
        ASSERT(n == 3u,                      "9000B → 3 chunks in seg 0");
        ASSERT(seg_pkts[0].seg_id    == 0,   "seg_id=0");
        ASSERT(seg_pkts[0].seg_total == 1,   "seg_total=1");
        ASSERT(seg_pkts[0].enc == GEO_WALK[0], "chunk 0 enc correct");
        ASSERT(seg_pkts[2].size == 808u,     "last chunk size=808");
        /* crc check */
        int crc_ok=1;
        for(int i=0;i<3;i++){
            if(_tstream_crc16(seg_pkts[i].data, seg_pkts[i].size) != seg_pkts[i].crc16)
                { crc_ok=0; break; }
        }
        ASSERT(crc_ok, "all CRCs correct");
    }

    /* T4: slice 2-segment file, verify boundary */
    printf("\n[T4] slice_seg 2-segment file boundary\n");
    {
        /* 720 chunks + 100 bytes = seg0 full (720 chunks), seg1 has 1 chunk */
        uint32_t fsize = 720u * 4096u + 100u;
        static uint8_t big[720 * 4096 + 100];
        for(uint32_t i=0;i<fsize;i++) big[i]=(uint8_t)((i*3) & 0xFF);

        uint16_t seg_tot = (uint16_t)tstream_seg_count(fsize);
        ASSERT(seg_tot == 2u, "seg_count=2");

        uint16_t n0 = tstream_slice_seg(seg_pkts, big, fsize, 0, seg_tot);
        ASSERT(n0 == 720u,  "seg 0: 720 chunks");
        ASSERT(seg_pkts[0].seg_id==0 && seg_pkts[0].seg_total==2, "seg0 header correct");
        ASSERT(seg_pkts[719].size==4096u, "seg0 last chunk full");

        uint16_t n1 = tstream_slice_seg(seg_pkts, big, fsize, 1, seg_tot);
        ASSERT(n1 == 1u,    "seg 1: 1 chunk");
        ASSERT(seg_pkts[0].seg_id==1 && seg_pkts[0].seg_total==2, "seg1 header correct");
        ASSERT(seg_pkts[0].size==100u, "seg1 chunk size=100");
        /* verify data at boundary */
        uint32_t off = 720u * 4096u;
        ASSERT(memcmp(seg_pkts[0].data, big+off, 100u)==0, "seg1 data at boundary correct");
    }

    /* T5: recv_seg + reconstruct 2-segment round-trip */
    printf("\n[T5] 2-segment round-trip reconstruct\n");
    {
        uint32_t fsize = 720u * 4096u + 500u;
        static uint8_t src[720 * 4096 + 500];
        for(uint32_t i=0;i<fsize;i++) src[i]=(uint8_t)((i*7+13) & 0xFF);

        uint16_t seg_tot = (uint16_t)tstream_seg_count(fsize);
        uint16_t n_per_seg[2];

        for(uint16_t s=0; s<seg_tot; s++){
            tring_init(&rings[s]);
            memset(stores[s], 0, sizeof(stores[s]));
            n_per_seg[s] = tstream_slice_seg(seg_pkts, src, fsize, s, seg_tot);
            /* recv all pkts for this segment */
            for(uint16_t i=0; i<n_per_seg[s]; i++)
                tstream_recv_seg(&rings[s], stores[s], &seg_pkts[i]);
        }

        memset(out_buf, 0, fsize);
        uint32_t written = tstream_reconstruct_segmented(
            rings,
            (const TStreamChunk (*)[TSTREAM_MAX_PKTS])stores,
            n_per_seg, seg_tot, out_buf);

        ASSERT(written == fsize, "reconstructed exact file size");
        ASSERT(memcmp(out_buf, src, fsize)==0, "byte-perfect match");
    }

    /* T6: recv out-of-order within segment */
    printf("\n[T6] recv out-of-order within segment\n");
    {
        static uint8_t file[12288]; /* 3 chunks */
        for(int i=0;i<12288;i++) file[i]=(uint8_t)(i & 0xFF);
        uint16_t seg_tot = 1u;
        tstream_slice_seg(seg_pkts, file, 12288u, 0, seg_tot);

        tring_init(&rings[0]);
        memset(stores[0], 0, sizeof(stores[0]));

        /* recv in reverse: chunk 2 first */
        tstream_recv_seg(&rings[0], stores[0], &seg_pkts[2]);
        tstream_recv_seg(&rings[0], stores[0], &seg_pkts[1]);
        tstream_recv_seg(&rings[0], stores[0], &seg_pkts[0]);

        uint16_t n_ps[1] = {3};
        memset(out_buf, 0, 12288);
        uint32_t wr = tstream_reconstruct_segmented(
            rings,
            (const TStreamChunk (*)[TSTREAM_MAX_PKTS])stores,
            n_ps, 1, out_buf);
        ASSERT(wr == 12288u,                      "recv reverse: size correct");
        ASSERT(memcmp(out_buf, file, 12288u)==0,  "recv reverse: byte-perfect");
    }

    /* T7: CRC fail in segmented recv → rejected */
    printf("\n[T7] CRC fail in seg pkt\n");
    {
        static uint8_t file[4096]; memset(file, 0xBB, 4096);
        tstream_slice_seg(seg_pkts, file, 4096u, 0, 1u);
        seg_pkts[0].data[0] ^= 0xFF;  /* corrupt */
        tring_init(&rings[0]);
        memset(stores[0], 0, sizeof(stores[0]));
        int res = tstream_recv_seg(&rings[0], stores[0], &seg_pkts[0]);
        ASSERT(res == -2, "corrupted seg pkt returns -2");
    }

    /* T8: 3-segment file */
    printf("\n[T8] 3-segment file (~8.6MB)\n");
    {
        uint32_t fsize = 720u*4096u*2u + 50000u;  /* 2 full segs + partial */
        static uint8_t src3[720*4096*2 + 50000];
        for(uint32_t i=0;i<fsize;i++) src3[i]=(uint8_t)((i+1) & 0xFF);

        uint16_t seg_tot = (uint16_t)tstream_seg_count(fsize);
        ASSERT(seg_tot == 3u, "3 segments");

        uint16_t n_ps[3];
        for(uint16_t s=0; s<3; s++){
            tring_init(&rings[s]);
            memset(stores[s], 0, sizeof(stores[s]));
            n_ps[s] = tstream_slice_seg(seg_pkts, src3, fsize, s, seg_tot);
            for(uint16_t i=0; i<n_ps[s]; i++)
                tstream_recv_seg(&rings[s], stores[s], &seg_pkts[i]);
        }

        memset(out_buf, 0, fsize);
        uint32_t wr = tstream_reconstruct_segmented(
            rings,
            (const TStreamChunk (*)[TSTREAM_MAX_PKTS])stores,
            n_ps, 3u, out_buf);
        ASSERT(wr == fsize,                       "3-seg: size correct");
        ASSERT(memcmp(out_buf, src3, fsize)==0,   "3-seg: byte-perfect");
    }

    printf("\n══════════════════════════════════\n");
    printf("Result: %d/%d passed%s\n", pass, pass+fail, fail==0?"  ✅ ALL PASS":"  ❌");
    return fail?1:0;
}
