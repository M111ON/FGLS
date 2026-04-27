/*
 * test_fec.c — Phase 9b: stride-3 A+B FEC Unit Tests
 * ════════════════════════════════════════════════════
 * T1: no loss
 * T2: 1 loss/block (all 60 blocks) → P0 path
 * T3: 2 loss/block recoverable  → stride-3 isolation path
 * T4: 2 loss/block unrecoverable → graceful fail, no corruption
 * T5: random scatter 20% (seed=0xDEADBEEF)
 * T6: all 66 pairs in one block → exhaustive 2-loss coverage
 *
 * Build: gcc -O2 -o test_fec test_fec.c
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "geo_temporal_lut.h"
#include "geo_temporal_ring.h"
#include "geo_pyramid.h"
#include "geo_tring_stream.h"
#include "geo_fec.h"

static int pass_count=0, fail_count=0;
#define ASSERT(c,m) do{ if(c){printf("  [PASS] %s\n",m);pass_count++;} \
                        else{printf("  [FAIL] %s\n",m);fail_count++;} }while(0)

/* ── static pools (avoid large stack) ─────────── */
static TStreamChunk store_orig[FEC_TOTAL_DATA];
static TStreamChunk store_work[FEC_TOTAL_DATA];
static FECParity    parity[FEC_TOTAL_PARITY];
static TRingCtx     ring;

static void fill_store(uint16_t n){
    for(uint16_t i=0;i<n;i++){
        uint16_t sz=(i==n-1u)?1337u:TSTREAM_DATA_BYTES;
        store_orig[i].size=sz;
        for(uint16_t b=0;b<sz;b++)
            store_orig[i].data[b]=(uint8_t)((i*7u+b*13u+42u)&0xFF);
        memset(store_orig[i].data+sz,0,TSTREAM_DATA_BYTES-sz);
    }
}
static void mark_all(uint16_t n){
    tring_init(&ring);
    for(uint16_t i=0;i<n;i++) ring.slots[i].present=1u;
}
static void drop(uint16_t pos){
    ring.slots[pos].present=0u;
    memset(store_work[pos].data,0,TSTREAM_DATA_BYTES);
    store_work[pos].size=0u;
}
static int stores_eq(uint16_t n){
    for(uint16_t i=0;i<n;i++){
        if(store_work[i].size!=store_orig[i].size) return 0;
        if(memcmp(store_work[i].data,store_orig[i].data,store_orig[i].size)!=0) return 0;
    }
    return 1;
}
static void reset_work(uint16_t n){
    memcpy(store_work,store_orig,sizeof(TStreamChunk)*n);
    mark_all(n);
}

/* ════════════════════════════════════════════════
 * T1: No Loss
 * ════════════════════════════════════════════════ */
static void t1_no_loss(void){
    printf("\n[T1] No Loss\n");
    uint16_t n=FEC_TOTAL_DATA;
    fill_store(n); reset_work(n);
    fec_encode_all(store_work,parity);
    uint16_t rec=fec_recover_all(&ring,store_work,parity);
    uint8_t gmap[FEC_TOTAL_DATA];
    uint16_t gaps=fec_gap_map(&ring,n,gmap);
    ASSERT(rec==0,  "0 recovered (nothing to do)");
    ASSERT(gaps==0, "0 gaps");
    ASSERT(stores_eq(n), "store byte-perfect");
}

/* ════════════════════════════════════════════════
 * T2: 1 loss per block — P0 path
 * ════════════════════════════════════════════════ */
static void t2_one_per_block(void){
    printf("\n[T2] 1 Loss per Block (60 losses)\n");
    uint16_t n=FEC_TOTAL_DATA;
    fill_store(n); reset_work(n);
    fec_encode_all(store_work,parity);
    /* drop last chunk in each block */
    for(uint8_t l=0;l<FEC_LEVELS;l++)
        for(uint8_t b=0;b<FEC_BLOCKS_PER_LEVEL;b++)
            drop((uint16_t)(l*GEO_PYR_PHASE_LEN+b*FEC_CHUNKS_PER_BLOCK+FEC_CHUNKS_PER_BLOCK-1u));
    uint8_t gmap[FEC_TOTAL_DATA];
    uint16_t gb=fec_gap_map(&ring,n,gmap);
    uint16_t rec=fec_recover_all(&ring,store_work,parity);
    uint16_t ga=fec_gap_map(&ring,n,gmap);
    ASSERT(gb==60,  "60 gaps before");
    ASSERT(rec==60, "60 recovered");
    ASSERT(ga==0,   "0 gaps after");
    ASSERT(stores_eq(n), "byte-perfect");
}

/* ════════════════════════════════════════════════
 * T3: 2 loss per block — recoverable pairs
 * Drop indices (0,1) per block → S3A[0]=1 S3A[1]=0 → isolated
 * ════════════════════════════════════════════════ */
static void t3_two_recoverable(void){
    printf("\n[T3] 2 Loss per Block — recoverable pair (0,1) x 60\n");
    uint16_t n=FEC_TOTAL_DATA;
    fill_store(n); reset_work(n);
    fec_encode_all(store_work,parity);
    for(uint8_t l=0;l<FEC_LEVELS;l++)
        for(uint8_t b=0;b<FEC_BLOCKS_PER_LEVEL;b++){
            uint16_t base=(uint16_t)(l*GEO_PYR_PHASE_LEN+b*FEC_CHUNKS_PER_BLOCK);
            drop(base+0); drop(base+1);
        }
    uint8_t gmap[FEC_TOTAL_DATA];
    uint16_t gb=fec_gap_map(&ring,n,gmap);
    uint16_t rec=fec_recover_all(&ring,store_work,parity);
    uint16_t ga=fec_gap_map(&ring,n,gmap);
    ASSERT(gb==120, "120 gaps before (2/block × 60)");
    ASSERT(rec==120,"120 recovered");
    ASSERT(ga==0,   "0 gaps after");
    ASSERT(stores_eq(n),"byte-perfect");
}

/* ════════════════════════════════════════════════
 * T4: 2 loss per block — unrecoverable pair
 * Drop indices (2,5) → both in implicit phase-2 {2,5,8,11}
 * P0: sees both → can't isolate
 * P1(S3A): 2→no, 5→no → residual=0, no info
 * P2(S3B): 2→no, 5→no → residual=0, no info
 * ════════════════════════════════════════════════ */
static void t4_two_unrecoverable(void){
    printf("\n[T4] 2 Loss per Block — unrecoverable pair (2,5) × 60\n");
    uint16_t n=FEC_TOTAL_DATA;
    fill_store(n); reset_work(n);
    fec_encode_all(store_work,parity);
    for(uint8_t l=0;l<FEC_LEVELS;l++)
        for(uint8_t b=0;b<FEC_BLOCKS_PER_LEVEL;b++){
            uint16_t base=(uint16_t)(l*GEO_PYR_PHASE_LEN+b*FEC_CHUNKS_PER_BLOCK);
            drop(base+2); drop(base+5);
        }
    uint8_t gmap[FEC_TOTAL_DATA];
    uint16_t gb=fec_gap_map(&ring,n,gmap);
    uint16_t rec=fec_recover_all(&ring,store_work,parity);
    uint16_t ga=fec_gap_map(&ring,n,gmap);
    /* verify no silent corruption in non-dropped slots */
    int clean=1;
    for(uint16_t i=0;i<n;i++){
        uint8_t l2=(uint8_t)(i/GEO_PYR_PHASE_LEN);
        uint8_t b2=(uint8_t)((i%GEO_PYR_PHASE_LEN)/FEC_CHUNKS_PER_BLOCK);
        uint8_t bi=(uint8_t)(i%FEC_CHUNKS_PER_BLOCK);
        if(bi==2||bi==5) continue; /* known dropped */
        if(store_work[i].size!=store_orig[i].size||
           memcmp(store_work[i].data,store_orig[i].data,store_orig[i].size)!=0){
            printf("  [DBG] corruption at slot %u (l=%u b=%u bi=%u)\n",i,l2,b2,bi);
            clean=0; break;
        }
    }
    ASSERT(gb==120, "120 gaps before");
    ASSERT(rec==0,  "0 recovered (graceful fail)");
    ASSERT(ga==120, "120 gaps remain");
    ASSERT(clean,   "no silent corruption");
}

/* ════════════════════════════════════════════════
 * T5: random scatter 20% (seed=0xDEADBEEF)
 * ════════════════════════════════════════════════ */
static void t5_random_scatter(void){
    printf("\n[T5] Random Scatter 20%% (seed=0xDEADBEEF)\n");
    uint16_t n=FEC_TOTAL_DATA;
    fill_store(n); reset_work(n);
    fec_encode_all(store_work,parity);

    uint8_t dropped[FEC_TOTAL_DATA]={0}; uint16_t dc=0;
    uint32_t rng=0xDEADBEEFu;
    while(dc<144u){
        rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;
        uint16_t pos=(uint16_t)(rng%n);
        if(!dropped[pos]){dropped[pos]=1;drop(pos);dc++;}
    }
    uint8_t gmap[FEC_TOTAL_DATA];
    uint16_t gb=fec_gap_map(&ring,n,gmap);
    uint16_t rec=fec_recover_all(&ring,store_work,parity);
    uint16_t ga=fec_gap_map(&ring,n,gmap);

    /* verify recovered bytes */
    int rec_ok=1;
    for(uint16_t i=0;i<n;i++){
        if(dropped[i]&&ring.slots[i].present){
            if(store_work[i].size!=store_orig[i].size||
               memcmp(store_work[i].data,store_orig[i].data,store_orig[i].size)!=0){
                rec_ok=0; break;
            }
        }
    }
    int intact=1;
    for(uint16_t i=0;i<n;i++){
        if(!dropped[i]){
            if(store_work[i].size!=store_orig[i].size||
               memcmp(store_work[i].data,store_orig[i].data,store_orig[i].size)!=0){
                intact=0; break;
            }
        }
    }
    printf("  [INFO] dropped=%u  recovered=%u  remaining=%u\n",gb,rec,ga);
    ASSERT(gb==144,            "144 gaps introduced");
    ASSERT(rec>11,             "recovered > 9a baseline (>11)"); /* 9a=11 for this seed */
    ASSERT(ga==gb-rec,         "gaps_after = dropped - recovered");
    ASSERT(rec_ok,             "recovered bytes byte-perfect");
    ASSERT(intact,             "non-dropped slots untouched");
}

/* ════════════════════════════════════════════════
 * T6: exhaustive 2-loss — all 66 pairs, block 0
 * Counts actually-recovered vs expected by scheme
 * ════════════════════════════════════════════════ */
static void t6_exhaustive_pairs(void){
    printf("\n[T6] Exhaustive 2-loss — all 66 pairs in block 0\n");
    uint16_t n=FEC_TOTAL_DATA;
    fill_store(n);

    /* pre-compute which pairs stride-3 A+B can theoretically recover */
    static const uint8_t S3A[12]={1,0,0,1,0,0,1,0,0,1,0,0};
    static const uint8_t S3B[12]={0,1,0,0,1,0,0,1,0,0,1,0};
    /* can recover if any parity isolates one: diff in at least one mask */
    int theory_rec=0;
    for(int i=0;i<12;i++) for(int j=i+1;j<12;j++){
        /* P0: both in mask → 1,1: not isolated */
        /* P1(S3A): isolated if S3A[i]!=S3A[j] */
        /* P2(S3B): isolated if S3B[i]!=S3B[j] */
        if(S3A[i]!=S3A[j]||S3B[i]!=S3B[j]) theory_rec++;
    }

    int actual_rec=0, actual_corrupt=0;
    for(int i=0;i<12;i++) for(int j=i+1;j<12;j++){
        reset_work(n);
        fec_encode_all(store_work,parity);
        drop((uint16_t)i); drop((uint16_t)j);

        uint16_t rec=fec_recover_all(&ring,store_work,parity);
        if(rec==2) {
            actual_rec++;
            /* verify both recovered correctly */
            if(store_work[i].size!=store_orig[i].size||
               memcmp(store_work[i].data,store_orig[i].data,store_orig[i].size)!=0||
               store_work[j].size!=store_orig[j].size||
               memcmp(store_work[j].data,store_orig[j].data,store_orig[j].size)!=0)
                actual_corrupt++;
        }
    }
    printf("  [INFO] theory_recoverable=%d/66  actual_recovered=%d/66  corrupt=%d\n",
           theory_rec, actual_rec, actual_corrupt);
    ASSERT(actual_rec==theory_rec, "actual == theoretical coverage");
    ASSERT(actual_corrupt==0,      "zero corruption in recovered pairs");
}

/* ── main ──────────────────────────────────────── */
int main(void){
    printf("════════════════════════════════════════════════\n");
    printf("Phase 9b — FEC stride-3 A+B Unit Tests\n");
    printf("Layout: %u data | %u parity | %u per block\n",
           FEC_TOTAL_DATA, FEC_TOTAL_PARITY, FEC_PARITY_PER_BLOCK);
    printf("Scheme: P0=all P1=(0,3,6,9) P2=(1,4,7,10)\n");
    printf("════════════════════════════════════════════════\n");

    t1_no_loss();
    t2_one_per_block();
    t3_two_recoverable();
    t4_two_unrecoverable();
    t5_random_scatter();
    t6_exhaustive_pairs();

    printf("\n════════════════════════════════════════════════\n");
    printf("Result: %d/%d PASS\n", pass_count, pass_count+fail_count);
    if(fail_count==0) printf("Phase 9b SOLID ✅\n");
    else              printf("FAILURES: %d ❌\n", fail_count);
    printf("════════════════════════════════════════════════\n");
    return fail_count==0?0:1;
}
