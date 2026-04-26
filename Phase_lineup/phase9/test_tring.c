#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "geo_temporal_lut.h"
#include "geo_temporal_ring.h"

static int pass=0,fail=0;
#define ASSERT(c,m) do{if(c){printf("  ✓ %s\n",m);pass++;}else{printf("  ✗ FAIL: %s\n",m);fail++;}}while(0)

int main(void){
    printf("╔══════════════════════════════╗\n");
    printf("║  Phase 7 — TRing Tests       ║\n");
    printf("╚══════════════════════════════╝\n");

    /* T1: walk 720 unique */
    printf("\n[T1] walk 720 unique\n");
    {
        int dup=0;
        for(int i=0;i<720;i++) for(int j=i+1;j<720;j++)
            if(GEO_WALK[i]==GEO_WALK[j]){dup++;break;}
        ASSERT(dup==0,"GEO_WALK all unique");
    }

    /* T2: pair involution pair(pair(v))==v */
    printf("\n[T2] pair involution\n");
    {
        int ok=1;
        for(int i=0;i<720;i++)
            if(TRING_CPAIR(GEO_PAIR[i])!=GEO_WALK[i]){ok=0;break;}
        ASSERT(ok,"cpair(cpair(v))==v for all 720");
    }

    /* T3: reverse index O(1) */
    printf("\n[T3] reverse index\n");
    {
        int ok=1;
        for(int i=0;i<720;i++){
            uint16_t pos=tring_pos(GEO_WALK[i]);
            if(pos!=i){ok=0;break;}
        }
        ASSERT(ok,"GEO_WALK_IDX[enc]==i for all i");
    }

    /* T4: pair routing — key → instant partner */
    printf("\n[T4] pair routing\n");
    {
        int ok=1;
        for(int i=0;i<720;i++){
            uint16_t ppos=tring_pair_pos(GEO_WALK[i]);
            if(ppos>=720){ok=0;break;}
            /* pair of pair must come back */
            uint16_t back=tring_pair_pos(GEO_WALK[ppos]);
            if(back!=i){ok=0;break;}
        }
        ASSERT(ok,"pair(pair(pos))==pos for all 720");
    }

    /* T5: chunk assign + gap detect */
    printf("\n[T5] self-healing gap detect\n");
    {
        TRingCtx r; tring_init(&r);
        /* assign chunks 0..9, skip pos 5 */
        for(int i=0;i<10;i++) if(i!=5) tring_assign(&r,i,i*100);
        uint16_t gap=tring_first_gap(&r);
        ASSERT(gap==5,"first gap at pos 5");
        /* fill it */
        tring_assign(&r,5,500);
        gap=tring_first_gap(&r);
        /* gap should now be >=10 */
        ASSERT(gap>=10,"gap resolved after fill");
    }

    /* T6: snap out-of-order chunk */
    printf("\n[T6] snap disorder\n");
    {
        TRingCtx r; tring_init(&r);
        r.head=0;
        /* chunk arrives at pos 10 (jumped over 1..9) */
        int g=tring_snap(&r,GEO_WALK[10]);
        ASSERT(g==10,"snap reports gap=10");
        ASSERT(r.head==10,"head snapped to 10");
        ASSERT(r.missing==10,"missing count=10");
    }

    /* T7: position = time (head advances = clock ticks) */
    printf("\n[T7] position = time\n");
    {
        TRingCtx r; tring_init(&r);
        for(int i=0;i<144;i++) tring_tick(&r);
        ASSERT(r.head==144,"head=144 after 144 ticks (1 FLUSH cycle)");
        for(int i=0;i<576;i++) tring_tick(&r);
        ASSERT(r.head==0,"head wraps to 0 after 720 ticks");
    }

    /* T8: tring_is_valid_next — pure predicate, no mutation */
    printf("\n[T8] is_valid_next predicate\n");
    {
        TRingCtx r; tring_init(&r);
        r.head=0;
        /* head=0, expected next=1 → enc at walk[1] must pass */
        ASSERT(tring_is_valid_next(&r,GEO_WALK[1])==1, "walk[1] valid when head=0");
        /* enc at walk[2] must fail */
        ASSERT(tring_is_valid_next(&r,GEO_WALK[2])==0, "walk[2] invalid when head=0");
        /* enc at walk[0] must fail (going backward) */
        ASSERT(tring_is_valid_next(&r,GEO_WALK[0])==0, "walk[0] invalid when head=0");
        /* state must be untouched */
        ASSERT(r.head==0, "head unchanged after predicate calls");
    }

    /* T9: tring_verify_next — check then snap, disorder rejected */
    printf("\n[T9] verify_next ordered ingress\n");
    {
        TRingCtx r; tring_init(&r);
        r.head=0;
        /* correct order: walk[1] → accepted */
        int res=tring_verify_next(&r,GEO_WALK[1]);
        ASSERT(res==1,      "verify_next returns 1 for on-path enc");
        ASSERT(r.head==1,   "head advanced to 1 after verify_next");
        /* disorder: walk[5] arrives next → rejected, head stays */
        res=tring_verify_next(&r,GEO_WALK[5]);
        ASSERT(res==0,      "verify_next returns 0 for out-of-order enc");
        ASSERT(r.head==1,   "head NOT moved on disorder");
        /* unknown enc: 0xFFFF → -1 */
        res=tring_verify_next(&r,0xFFFFu);
        ASSERT(res==-1,     "verify_next returns -1 for unknown enc");
        /* continue correct: walk[2] → accepted */
        res=tring_verify_next(&r,GEO_WALK[2]);
        ASSERT(res==1,      "verify_next resumes after disorder");
        ASSERT(r.head==2,   "head=2 after resume");
    }

    printf("\n══════════════════════════════════\n");
    printf("Result: %d/%d passed%s\n",pass,pass+fail,fail==0?"  ✅ ALL PASS":"  ❌");
    return fail?1:0;
}
