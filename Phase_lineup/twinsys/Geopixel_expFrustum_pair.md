# FGLS × LC × TPOGLS Integration — Session Handoff
> Claude Sonnet 4.6 | Session boundary handoff

---

## Session Summary

ค้นพบว่า FGLS, LC, และ TPOGLS มี **shared geometric DNA** ที่ไม่บังเอิญ
และ LC `lc_twin_gate.h` ถูก integrate เข้า `pogls_twin_bridge.h` แล้ว (patched version แนบมาด้วย)

---

## Geometric Invariants (Load-bearing — ห้ามเปลี่ยน)

```
27,648 = 4^5 × 3^3    ← LC / FGLS total node space
 3,456 = 2^7 × 3^3    ← TPOGLS GEO_FULL_N (icosphere slots)
    54 = 2  × 3^3      ← icosphere faces = 6 frustum × 9 cosets
   144 = F(12)         ← fibo clock = 4608B ÷ 32 threads
     6 = GEO_SPOKES    = APEX_FRUSTUM_MOUNT = LC faces per cube

shared invariant: 3^3 = 27 (ternary routing depth)
LC/FGLS resolution: 4^5 = 1024 (vs TPOGLS 2^7 = 128) → ratio = 2^3 = 8×
```

---

## What Was Done This Session

### 1. `lc_twin_gate.h` (NEW — ทำแล้ว ✅)
Gate layer ระหว่าง `pipeline_wire_process` และ `geo_fast_intersect`

```
twin_bridge_write(addr, value)
  → pipeline_wire_process        (existing)
  → [lc_twin_gate]               ← NEW: LC polarity gate
      WARP      → geo_fast_intersect fast path
      ROUTE     → geo_fast_intersect normal
      COLLISION → drift_acc += 8, then geo_fast_intersect
      GROUND    → bypass geo entirely → dodeca_insert ทันที
  → geo_fast_intersect           (existing)
  → dodeca_insert                (existing)
```

encode logic:
- `hA` = encode addr จาก bits จริง (sign/mag/rgb/level/angle)
- `hB` = encode value จาก bits จริง (ไม่ใช่ complement สำเร็จรูป)
- `lch_gate(hA, hB, palette, palette)` judge decision

### 2. `pogls_twin_bridge.h` (PATCHED ✅)
5 จุดที่แก้:
1. `#include "lc_twin_gate.h"`
2. `LCTwinGateCtx lc_gate` ใน TwinBridge struct
3. `lc_twin_gate_init()` ใน `twin_bridge_init`
4. GROUND path bypass + COLLISION drift bump ใน `twin_bridge_write`
5. `TwinBridgeStats` เพิ่ม lc_warp/route/ground/collision counters

---

## GROUND Path — Known Gap (Next Priority)

ปัจจุบัน GROUND path encode `ground_addr` แบบ naive:
```c
uint64_t ground_addr = raw & UINT64_C(0x00FFFFFFFFFFFFFF);  // ← wrong
```

ควรใช้ **3^3 trit decomposition** map เข้า dodeca territory จริงๆ:
```c
uint32_t trit   = (uint32_t)((addr ^ value) % 27u);  // 3^3 ternary index
uint8_t  spoke  = (uint8_t)(trit % 6u);               // GEO_SPOKES=6
uint8_t  offset = (uint8_t)(trit % 9u);               // coset sub-index
// dodeca_insert(&b->dodeca, trit_mapped_addr, spoke, 0, offset, 0, 0)
```

---

## FGLS Architecture (ที่เพิ่งอ่าน)

```
FrustumNode (6 directions: X+/X-/Y+/Y-/Z+/Z-)
  + CoreCube (3-pair X→Y→Z sequential lock)
  = BigCube

BigCube × 12 cosets = GiantArray (4,608B payload)
  serialize → CubeFileStore (4,896B = 34×144)

"Delete" = reserved_mask bitmask (cosets 9-11)
  → payload zero-filled for masked cosets
  → GPU stride skips masked slots
  → data reconstructible from seed, path inaccessible
```

Key file: `geo_cube_file_store.h`
```c
CubeFileHeader.reserved_mask  // uint32_t bitmask — this IS the delete mechanism
```

---

## Integration Stack (Full Vision)

```
[twin_bridge_write(addr, value)]
         ↓
[pipeline_wire_process]           ← existing POGLS pipeline
         ↓
[lc_twin_gate]                    ← DONE: LC polarity gate
    WARP/ROUTE/COLLISION/GROUND
         ↓
[geo_fast_intersect + dodeca]     ← existing Twin Geo
         ↓
[GiantArray / CubeFileStore]      ← NEXT: FGLS storage layer
         ↓
[reserved_mask delete]            ← "delete" via structural silence
```

---

## Next Session Tasks (Priority Order)

### P0 — Fix GROUND path (trit mapping)
แก้ใน `pogls_twin_bridge.h` GROUND branch:
```c
// ใช้ trit decomposition แทน raw mask
uint32_t trit = (uint32_t)((addr ^ value) % 27u);
```

### P1 — `fgls_twin_store.h` (NEW file)
Bridge ระหว่าง `dodeca_insert` output กับ `GiantArray`:
- `dodeca_insert` → map output → `GiantArray.cubes[coset].faces[face]`
- coset = `trit / 6` (0-3 of 9 active) 
- face = `trit % 6` (0-5 = 6 frustum directions)
- serialize → `CubeFileStore` (4,896B)

### P2 — Delete API
```c
void twin_bridge_delete(TwinBridge *b, uint64_t addr);
// → compute trit → coset
// → set reserved_mask bit ใน CubeFileStore header
// → future writes to addr → GROUND_ABSORB via palette filter
```

### P3 — LetterCube coupling ใน twin_bridge
`geo_letter_cube.h`: `lc_pair_match()` ใช้ `angle_key ^ slope_hash`
→ อาจ feed จาก `fibo_clock` seed เป็น `slope_hash`
→ LetterPair index = `addr % 26` (A-Z)

---

## File Index (แนบมาใน package นี้)

```
handoff_pkg/
├── SESSION_HANDOFF.md          ← this file
├── fgls_core/
│   ├── geo_frustum_node.h      ← FrustumNode + fibo slope L0..L3
│   ├── geo_core_cube.h         ← 3-pair X→Y→Z sequential lock
│   ├── geo_big_cube.h          ← Core + FrustumPairs + neighbor handshake
│   ├── geo_giant_array.h       ← 12 GiantCubes + 4608B flat pack
│   ├── geo_cube_file_store.h   ← serialize + reserved_mask DELETE ←KEY
│   ├── geo_letter_cube.h       ← 6! pairing + 78-step closure
│   ├── geo_apex_wire.h         ← 4-wire 16B + 9 hilbert paths
│   ├── geo_rubik_drive.h       ← Rubik 54-cell base2×base3
│   ├── geo_primitives.h        ← derive_next_core (2-cycle engine)
│   └── geo_rewind.h            ← L2 temporal rewind (972 slots)
├── lc_src/
│   ├── lc_hdr.h                ← LCHdr pack/unpack, palette, lch_gate
│   ├── lc_wire.h               ← lcw_space, lcw_route, LCGate enum
│   ├── lc_fs.h                 ← GCFS chunk (4,896B), lc_fs_write
│   ├── lc_gcfs_wire.h          ← GCFS wire protocol
│   └── lc_delete.h             ← ghost node, triple-cut mechanism
└── tpogls_patched/
    ├── pogls_twin_bridge.h     ← PATCHED: LC gate wired in
    └── lc_twin_gate.h          ← NEW: LC polarity gate header
```

---

## Sacred Numbers (DO NOT CHANGE)

| Value | Source | Used in |
|-------|--------|---------|
| 27,648 | 4^5 × 3^3 | LC/FGLS node space |
| 3,456 | 2^7 × 3^3 | GEO_FULL_N |
| 54 | 2 × 3^3 | icosphere faces / Rubik cells |
| 144 | F(12) | fibo clock / GCFS ÷ 32 threads |
| 6 | GEO_SPOKES | frustum directions |
| 27 | 3^3 | trit mapping index |
| 9 | 3^2 | GCFS_ACTIVE_COSETS |
| 78 | LCM(13,6) | LetterCube closure steps |

*3^3 = 27 คือ shared invariant ระหว่างทุก system*
