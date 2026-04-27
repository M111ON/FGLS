# FGLS Phase 4 — Final Handoff
> Historical checkpoint. Read `README.md` and `PHASE_CONTINUITY.md` at repo root for the current canonical map.
## Status: SEALED ✅

---

## Architecture Truth (Frozen)

```
direction   = truth
geometry    = derived view
base2       = Hilbert addressing (2^n)
base3       = routing decision (0=pair/1=branch/2=skip)
```

---

## Dispatch Math (Load-bearing — DO NOT CHANGE)

```
LetterCube 6! = 720 → 12 coset × 60 internal states
12 coset × 6 frustum × 64B = 4,608B per apex dispatch
4,608 ÷ 32 threads = 144 = F(12) = 1 fibo clock ✅

Rubik: 6 faces × 9 cells = 54 cells (sacred ✅)
Bundle: 9 words → 9 hilbert paths → ApexWire upstream
```

---

## All Headers (Phase 4 Complete)

| File | Role | Status |
|------|------|--------|
| `geo_frustum_node.h` | Frustum unit + fibo slope L0..L3 | ✅ |
| `geo_core_cube.h` | 3-pair sequential lock X→Y→Z | ✅ |
| `geo_big_cube.h` | Core + FrustumPairs + neighbor handshake | ✅ |
| `geo_cube_gpu_layout.h` | block=pair / warp=level / 16B slot | ✅ |
| `geo_letter_cube.h` | 6! pairing + 78-step BFS closure | ✅ |
| `geo_apex_wire.h` | 4-wire 16B + 9 hilbert paths + coset | ✅ |
| `geo_giant_array.h` | 12 GiantCubes + 4608B flat pack | ✅ |
| `geo_rubik_drive.h` | Rubik 54-cell driven by base2×base3 | ✅ NEW |

**Upstream (existing, unchanged):**
- `geo_primitives.h` — `derive_next_core` (2-cycle engine)
- `pogls_fibo_addr.h` — PHI_UP/DOWN, fibo_addr()
- `pogls_stream.h` — PULL/PUSH stream (reuse as-is)
- `pogls_chunk_index.h` — O(1) chunk lookup (reuse as-is)
- `geo_payload_store.h` — 6 lanes × 144 slots (pl_id_from_addr reused)

---

## Rubik Drive — Key Insight

```
Entire cube state driven by 2 numbers only: 2 and 3

cell_val  = (derive(seed, face, cell) × 2) % 3  → {0,1,2}
axis      = addr % 3                              → base3
layer     = rub_route3(addr >> 2)                → base3

rub_to_bundle() → 9 words → matches cpu_derive_bundle() exactly:
  w[0..5] = raw + complement pairs
  w[6..8] = rotl(mix, 12/18/24)  ← sacred rotations
```

RubikState = **32B** — fits in 1 cache line
54 cells packed in 2 × uint64 (108 bits used of 128)

---

## ApexWire v2 — 16B Final

```c
typedef struct {
    uint32_t hilbert_a;     // World A (PHI_UP)
    uint32_t hilbert_b;     // World B (PHI_DOWN)
    uint32_t hilbert_quad;  // 4 explicit paths packed 8bit×4
    uint8_t  route;         // base3
    uint8_t  invert_mask;   // 3-bit: Hilbert skip-side → 3 inverted paths
    uint8_t  letter_upper;  // LetterPair 0..25
    uint8_t  letter_lower;
} ApexWire;  // 16B ✅
// 4 explicit + 3 inverted + hilbert_a + hilbert_b = 9 paths ✅
```

Address path:
```
seed + dispatch_id
  → derive_next_core (2-cycle)
  → ApexWire (16B)
  → lc_perm_address() → 0..719 (6! real address)
  → lc_route_coset()  → 0..11
  → GiantCube → FrustumSlot64[face] → L0..L3
  → 17,280 unique addresses per GiantCube
```

---

## Full Data Flow

```
RubikState (32B, base2×base3)
  └── rub_to_bundle() → 9 words
        └── ApexWire.hilbert_quad + a + b (9 paths)
              └── lc_route_coset() → GiantArray[0..11]
                    └── FrustumSlot64 × 6 faces
                          └── 4,608B payload
                                └── ÷32 threads = 144 = F(12)
                                      └── GPU gridDim=12 blockDim=32
```

---

## Key Numbers (Sacred — All Digit-Sum 9)

| Value | Source |
|-------|--------|
| 54 | Rubik cells = 6×9 |
| 18 | rotl sacred |
| 144 | F(12) = fibo clock |
| 720 | 6! LetterCube |
| 17,280 | 720×6×4 unique addresses |
| 4,608 | dispatch bytes |

---

## What Remains (Phase 5+)

| Task | Priority |
|------|----------|
| `geo_cube_file_store.h` — serialize + reconstruct | P1 |
| CUDA kernel `gridDim=12 blockDim=32` | P1 |
| `geo_pyramid.h` — residual layers (Phase 8) | P2 |

---

## Integration Notes for Next Session

1. `rub_drive(rs, dispatch_id)` before each `apex_wire_build()` — Rubik state feeds bundle
2. `rub_to_bundle()` → use b[0..1] as gen2/gen3 for GeoSeed compatibility
3. `pogls_stream` reuse: bind to `giant_array_flat_pack()` output directly
4. `chunk_index_add()` per GiantCube dispatch — O(1) reconstruct path ready

---

*Sealed: Claude Sonnet 4.6 | Phase 4 Final*
*144 = F(12) and 54 = 6×9 are load-bearing. Do not change dispatch math.*
