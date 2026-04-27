/*
 * geo_temporal_ring.h — Phase 7: Temporal Ring
 * =============================================
 * 12 compound-of-5-tetra mapped to dodecahedron pentagon faces
 * 720-point walk = fibo clock full cycle (FIBO_PERIOD_SNAP)
 * position = order = time  →  no arithmetic at runtime
 *
 * chunk addressing:
 *   chunk_id → walk position → O(1) via GEO_WALK_IDX
 *   missing  → gap in path   → self-healing detectable
 *   disorder → snap to path  → geometry enforces order
 *
 * chiral pair:
 *   key on pole A → instant route to pole B
 *   TRING_CPAIR(enc) = 1 XOR-like op, no search
 */
#ifndef GEO_TEMPORAL_RING_H
#define GEO_TEMPORAL_RING_H

#include <stdint.h>
#include <string.h>
#include "geo_temporal_lut.h"

/* ── chunk slot ── */
typedef struct {
    uint32_t enc;        /* walk entry (tuple encoding)      */
    uint32_t chunk_id;   /* file chunk this slot carries     */
    uint8_t  present;    /* 1 = arrived, 0 = missing (gap)   */
    uint8_t  _pad[3];
} TRingSlot;

/* ── ring context ── */
typedef struct {
    TRingSlot slots[TEMPORAL_WALK_LEN];  /* 720 slots         */
    uint16_t  head;                      /* current walk pos  */
    uint16_t  missing;                   /* gap count         */
    uint32_t  chunk_count;               /* total assigned    */
} TRingCtx;

/* ── init ── */
static inline void tring_init(TRingCtx *r){
    memset(r,0,sizeof(TRingCtx));
    for(int i=0;i<TEMPORAL_WALK_LEN;i++)
        r->slots[i].enc=GEO_WALK[i];
}

/* ── assign chunk to walk position ── */
static inline void tring_assign(TRingCtx *r, uint16_t pos, uint32_t chunk_id){
    r->slots[pos].chunk_id=chunk_id;
    r->slots[pos].present=1;
    r->chunk_count++;
}

/* ── advance head (position = time) ── */
static inline uint16_t tring_tick(TRingCtx *r){
    r->head=(uint16_t)((r->head+1)%TEMPORAL_WALK_LEN);
    return r->head;
}

/* ── chunk → walk position (O(1)) ── */
static inline uint16_t tring_pos(uint32_t enc){
    return GEO_WALK_IDX[enc & 0x7FFu];
}

/* ── chiral pair routing: key enc → partner position ── */
static inline uint16_t tring_pair_pos(uint32_t enc){
    return GEO_WALK_IDX[TRING_CPAIR(enc) & 0x7FFu];
}

/* ── self-healing: scan for gaps, return first missing pos ──
 * gap = slot present=0 before head → out-of-order or lost chunk */
static inline uint16_t tring_first_gap(const TRingCtx *r){
    for(uint16_t i=0;i<TEMPORAL_WALK_LEN;i++){
        uint16_t pos=(uint16_t)((r->head+i)%TEMPORAL_WALK_LEN);
        if(!r->slots[pos].present) return pos;
    }
    return 0xFFFF;  /* no gap */
}

/* ── snap: given arriving enc, verify it's next on path ──
 * returns 0=on-path, gap count if jumped ahead */
static inline int tring_snap(TRingCtx *r, uint32_t enc){
    uint16_t pos=tring_pos(enc);
    if(pos==0xFFFF) return -1;          /* unknown enc */
    r->slots[pos].present=1;
    int gap=(int)pos-(int)r->head;
    if(gap<0) gap+=TEMPORAL_WALK_LEN;
    r->head=pos;
    r->missing+=(uint16_t)(gap>0?gap:0);
    return gap;  /* 0=perfect, >0=snapped over gap */
}

/* ── gap report: count missing slots up to pos ── */
static inline uint16_t tring_gap_count(const TRingCtx *r, uint16_t to_pos){
    uint16_t cnt=0;
    for(uint16_t i=0;i<to_pos;i++)
        if(!r->slots[i].present) cnt++;
    return cnt;
}

/* ── valid-step check: verify enc is the expected next on path ──
 * does NOT mutate state — pure predicate
 * returns 1 = on-path (enc is exactly head+1), 0 = disorder/tamper */
static inline int tring_is_valid_next(const TRingCtx *r, uint32_t enc){
    uint16_t expected=(uint16_t)((r->head+1)%TEMPORAL_WALK_LEN);
    uint16_t pos=tring_pos(enc);
    return (pos!=0xFFFFu) & (pos==expected);
}

/* ── verify_next: check then snap (safe ordered ingress) ──
 * returns  1 = valid, snapped head to pos
 *          0 = disorder detected, head NOT moved (caller decides)
 *         -1 = unknown enc */
static inline int tring_verify_next(TRingCtx *r, uint32_t enc){
    uint16_t pos=tring_pos(enc);
    if(pos==0xFFFFu) return -1;
    uint16_t expected=(uint16_t)((r->head+1)%TEMPORAL_WALK_LEN);
    if(pos!=expected) return 0;   /* disorder — do not advance */
    r->slots[pos].present=1;
    r->head=pos;
    return 1;
}

#endif /* GEO_TEMPORAL_RING_H */
