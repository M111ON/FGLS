/*
 * geo_cube_gpu_layout.h — FGLS GPU Dispatch Layout (Phase 4)
 * ════════════════════════════════════════════════════════════
 *
 * GPU work unit mapping:
 *
 *   1 FrustumPair  → 1 compute block   (spatial locality: same axis)
 *   1 Fibo level   → 1 warp            (parallel detail resolution)
 *
 * Block/thread layout (CUDA analogy):
 *
 *   gridDim.x  = CUBE_AXIS_COUNT (3)   ← X, Y, Z pairs
 *   blockDim.x = FRUSTUM_MAX_LEVELS (4) ← L0..L3 warps
 *   blockDim.y = 2                     ← pos / neg within pair
 *
 * Memory layout (coalesced access pattern):
 *
 *   [block 0: X pair]
 *     warp 0: L0 pos, L0 neg   ← core-level data (shared mem)
 *     warp 1: L1 pos, L1 neg   ← φ¹ ring
 *     warp 2: L2 pos, L2 neg   ← φ² ring
 *     warp 3: L3 pos, L3 neg   ← φ³ ring
 *   [block 1: Y pair] ...
 *   [block 2: Z pair] ...
 *
 * CubeGpuSlot: flat struct for DMA transfer to GPU
 *   size = 3 pairs × 2 dirs × 4 levels × 16 bytes = 384 bytes
 *   = 6 cache lines (64B each) → fits L1
 */

#ifndef GEO_CUBE_GPU_LAYOUT_H
#define GEO_CUBE_GPU_LAYOUT_H

#include <stdint.h>
#include "geo_big_cube.h"

/* ── GPU dispatch constants ──────────────────── */
#define GPU_GRID_X      CUBE_AXIS_COUNT      /* 3 blocks (X/Y/Z)      */
#define GPU_BLOCK_LEVEL FRUSTUM_MAX_LEVELS   /* 4 warps (L0..L3)      */
#define GPU_BLOCK_DIR   2u                   /* pos + neg per pair     */
#define GPU_SLOT_WORDS  (GPU_GRID_X * GPU_BLOCK_DIR * GPU_BLOCK_LEVEL)
                                             /* = 24 uint64 slots      */

/* ── GpuFrustumSlot: one level of one frustum ── */
typedef struct {
    uint64_t  core;     /* level core                                 */
    uint32_t  addr;     /* fibo addr at this level                    */
    uint8_t   axis;     /* X/Y/Z                                      */
    uint8_t   dir;      /* FRUSTUM_DIR_*                              */
    uint8_t   level;    /* 0..3                                       */
    uint8_t   world;    /* 0=expand, 1=shrink                         */
} GpuFrustumSlot;      /* 16 bytes — one GPU thread's workload        */

/* ── CubeGpuBlock: one axis pair = one compute block ── */
typedef struct {
    GpuFrustumSlot slots[GPU_BLOCK_DIR][GPU_BLOCK_LEVEL]; /* [dir][level] */
    uint64_t       master_core;  /* CoreCube master (in shared mem)   */
    uint8_t        axis;
    uint8_t        _pad[7];
} CubeGpuBlock;        /* 24×16 + 16 = 400 bytes → pad to 512 = 8 CL  */

/* ── CubeGpuPayload: full Big Cube GPU transfer ─ */
typedef struct {
    CubeGpuBlock  blocks[GPU_GRID_X];   /* X, Y, Z                   */
    uint64_t      cube_master_core;     /* final folded master_core   */
    uint32_t      cube_id;              /* BigCube identity tag       */
    uint32_t      _pad;
} CubeGpuPayload;

/* ── cube_gpu_pack: BigCube → CubeGpuPayload ─── */
static inline void cube_gpu_pack(const BigCube *bc,
                                 uint32_t cube_id,
                                 CubeGpuPayload *out)
{
    out->cube_master_core = bc->core.master_core;
    out->cube_id          = cube_id;

    for (uint8_t ax = 0; ax < CUBE_AXIS_COUNT; ax++) {
        CubeGpuBlock  *blk = &out->blocks[ax];
        const FrustumPair *fp = &bc->pairs[ax];
        blk->master_core = bc->core.master_core;
        blk->axis        = ax;

        /* dir 0 = pos, dir 1 = neg */
        const FrustumNode *fns[2] = { &fp->pos, &fp->neg };
        for (uint8_t d = 0; d < GPU_BLOCK_DIR; d++) {
            const FrustumNode *fn = fns[d];
            for (uint8_t lv = 0; lv < GPU_BLOCK_LEVEL; lv++) {
                GpuFrustumSlot *s = &blk->slots[d][lv];
                s->core  = fn->levels[lv].core;
                s->addr  = fn->levels[lv].addr;
                s->axis  = ax;
                s->dir   = fn->dir;
                s->level = lv;
                s->world = fn->levels[lv].world;
            }
        }
    }
}

/* ── cube_gpu_slot_idx: flat index helper ────── */
/* Returns linear index of slot: block*8 + dir*4 + level */
static inline uint32_t cube_gpu_slot_idx(uint8_t axis,
                                         uint8_t dir,
                                         uint8_t level)
{
    return (uint32_t)axis * (GPU_BLOCK_DIR * GPU_BLOCK_LEVEL)
         + (uint32_t)dir  *  GPU_BLOCK_LEVEL
         + (uint32_t)level;
}

#endif /* GEO_CUBE_GPU_LAYOUT_H */
