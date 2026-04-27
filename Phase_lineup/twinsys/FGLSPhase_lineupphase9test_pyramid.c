#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "geo_temporal_lut.h"
#include "geo_temporal_ring.h"
#include "geo_pyramid.h"

static int pass=0, fail=0;
#define ASSERT(c,m) do{if(c){printf("  ✓ %s\n",m);pass++;}else{printf("  ✗ FAIL: %s\n",m);fail++;}}while(0)

int main(void){
    printf("╔══════════════════════════════╗\n");
    printf("║  Phase 8 — Pyramid Tests     ║\n");
    printf("╚══════════════════════════════╝\n");

    /* T1: compile-time constants */
    printf("\n[T1] constants\n");
    ASSERT(GEO_PYR_DEPTH * GEO_PYR_PHASE_LEN == GEO_PYR_TOTAL, "5 × 144 = 720");
    ASSERT(GEO_PYR_TOTAL == TEMPORAL_WALK_LEN,                  "pyramid total = ring len");

    /* T2: decompose covers all 720 positions correctly */
    printf("\n[T2] pyr_decompose + compose roundtrip\n");
    {
        int ok=1;
        for(uint16_t p=0; p<720; p++){
            PyramidPos pp = pyr_decompose(p);
            if(pp.level != p/144 || pp.slot != p%144 || pp.pos != p){ ok=0; break; }
            if(pyr_compose(pp.level, pp.slot) != p){ ok=0; break; }
        }
        ASSERT(ok, "decompose/compose roundtrip all 720");
    }

    /* T3: phase bounds */
    printf("\n[T3] phase start/end bounds\n");
    {
        int ok=1;
        for(uint8_t l=0; l<5; l++){
            if(pyr_phase_start(l) != l*144)     { ok=0; break; }
            if(pyr_phase_end(l)   != l*144+143) { ok=0; break; }
        }
        ASSERT(ok, "phase start/end correct for L0-L4");
        ASSERT(pyr_phase_start(0)==0,   "L0 starts at 0");
        ASSERT(pyr_phase_end(4)==719,   "L4 ends at 719");
    }

    /* T4: pyr_head_level tracks ring head correctly */
    printf("\n[T4] pyr_head_level\n");
    {
        TRingCtx r; tring_init(&r);
        r.head = 0;   ASSERT(pyr_head_level(&r)==0, "head=0   → L0");
        r.head = 143; ASSERT(pyr_head_level(&r)==0, "head=143 → L0");
        r.head = 144; ASSERT(pyr_head_level(&r)==1, "head=144 → L1");
        r.head = 288; ASSERT(pyr_head_level(&r)==2, "head=288 → L2");
        r.head = 576; ASSERT(pyr_head_level(&r)==4, "head=576 → L4");
        r.head = 719; ASSERT(pyr_head_level(&r)==4, "head=719 → L4");
    }

    /* T5: pyr_is_level_transition */
    printf("\n[T5] level transition detection\n");
    {
        TRingCtx r; tring_init(&r);
        r.head = 0;   ASSERT(pyr_is_level_transition(&r)==1, "pos 0   = transition");
        r.head = 144; ASSERT(pyr_is_level_transition(&r)==1, "pos 144 = transition");
        r.head = 288; ASSERT(pyr_is_level_transition(&r)==1, "pos 288 = transition");
        r.head = 143; ASSERT(pyr_is_level_transition(&r)==0, "pos 143 ≠ transition");
        r.head = 145; ASSERT(pyr_is_level_transition(&r)==0, "pos 145 ≠ transition");
    }

    /* T6: pyr_scan_phase — empty ring → 144 residuals, no complete */
    printf("\n[T6] scan empty ring\n");
    {
        TRingCtx r; tring_init(&r);
        for(uint8_t l=0; l<5; l++){
            PyramidPhase ph = pyr_scan_phase(&r, l);
            if(ph.present!=0 || ph.residual!=144 || ph.complete!=0){ fail++;
                printf("  ✗ FAIL: empty L%d wrong\n",l); goto t6done; }
        }
        pass++; printf("  ✓ all 5 levels: present=0 residual=144 complete=0\n");
        t6done:;
    }

    /* T7: pyr_scan_phase — fill exactly one phase */
    printf("\n[T7] fill L2 completely\n");
    {
        TRingCtx r; tring_init(&r);
        for(uint16_t p=288; p<432; p++) tring_assign(&r, p, p*10u);
        PyramidPhase ph = pyr_scan_phase(&r, 2);
        ASSERT(ph.present==144,    "L2 present=144");
        ASSERT(ph.residual==0,     "L2 residual=0");
        ASSERT(ph.complete==1,     "L2 complete");
        ASSERT(ph.first_gap==0xFFFF, "L2 no gap");
        /* other levels untouched */
        PyramidPhase ph0 = pyr_scan_phase(&r, 0);
        ASSERT(ph0.present==0 && ph0.residual==144, "L0 still empty");
    }

    /* T8: pyr_scan_phase — partial fill, first_gap correct */
    printf("\n[T8] partial fill L0, first_gap\n");
    {
        TRingCtx r; tring_init(&r);
        /* fill slots 0..9, skip 10, fill 11..143 */
        for(uint16_t p=0; p<144; p++)
            if(p!=10) tring_assign(&r, p, p);
        PyramidPhase ph = pyr_scan_phase(&r, 0);
        ASSERT(ph.present==143,   "L0 present=143");
        ASSERT(ph.residual==1,    "L0 residual=1");
        ASSERT(ph.complete==0,    "L0 not complete");
        ASSERT(ph.first_gap==10,  "L0 first_gap=10");
    }

    /* T9: pyr_complete_levels bitmask */
    printf("\n[T9] pyr_complete_levels bitmask\n");
    {
        TRingCtx r; tring_init(&r);
        /* fill L1 (144..287) and L3 (432..575) */
        for(uint16_t p=144; p<288; p++) tring_assign(&r, p, p);
        for(uint16_t p=432; p<576; p++) tring_assign(&r, p, p);
        uint8_t mask = pyr_complete_levels(&r);
        ASSERT(mask == 0b00001010, "complete mask = L1+L3 = 0x0A");
        /* fill L0 too */
        for(uint16_t p=0; p<144; p++) tring_assign(&r, p, p);
        mask = pyr_complete_levels(&r);
        ASSERT(mask == 0b00001011, "complete mask = L0+L1+L3 = 0x0B");
    }

    /* T10: pyr_residual_count */
    printf("\n[T10] pyr_residual_count\n");
    {
        TRingCtx r; tring_init(&r);
        ASSERT(pyr_residual_count(&r)==720, "empty ring → 720 residuals");
        for(uint16_t p=0; p<144; p++) tring_assign(&r, p, p);
        ASSERT(pyr_residual_count(&r)==576, "after L0 fill → 576 residuals");
        for(uint16_t p=0; p<720; p++) tring_assign(&r, p, p);
        ASSERT(pyr_residual_count(&r)==0,   "full ring → 0 residuals");
    }

    /* T11: pyr_scan_all */
    printf("\n[T11] pyr_scan_all\n");
    {
        TRingCtx r; tring_init(&r);
        for(uint16_t p=0; p<288; p++) tring_assign(&r, p, p); /* fill L0+L1 */
        PyramidPhase all[5];
        pyr_scan_all(&r, all);
        ASSERT(all[0].complete==1, "scan_all: L0 complete");
        ASSERT(all[1].complete==1, "scan_all: L1 complete");
        ASSERT(all[2].complete==0, "scan_all: L2 incomplete");
        ASSERT(all[3].complete==0, "scan_all: L3 incomplete");
        ASSERT(all[4].complete==0, "scan_all: L4 incomplete");
    }

    printf("\n══════════════════════════════════\n");
    printf("Result: %d/%d passed%s\n", pass, pass+fail, fail==0?"  ✅ ALL PASS":"  ❌");
    return fail?1:0;
}
