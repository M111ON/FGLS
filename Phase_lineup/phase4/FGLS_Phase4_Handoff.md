# FGLS Phase 4 — Handoff Document
> Historical checkpoint. Read `README.md` and `PHASE_CONTINUITY.md` at repo root for the current canonical map.
## Status: LOCKED ✅

---

## Architecture Truth

```
direction = truth
geometry  = derived view
```

---

## Dispatch Math (Frozen)

```
LetterCube 6! = 720
  └── 12 coset × 60 internal states
        └── 12 coset × 6 frustum × 64B slot
            = 4,608B per apex dispatch
            ÷ 32 threads (warp)
            = 144 slots = F(12) = 1 fibo clock ✅
```

---

## Headers Delivered (Phase 4)

| File | Role | Status |
|------|------|--------|
| `geo_frustum_node.h` | Frustum unit + fibo slope L0..L3 | ✅ |
| `geo_core_cube.h` | 3-pair sequential lock X→Y→Z | ✅ |
| `geo_big_cube.h` | Core + 3 FrustumPairs + neighbor handshake | ✅ |
| `geo_cube_gpu_layout.h` | block=pair / warp=level / 16B slot | ✅ |
| `geo_letter_cube.h` | 6! pairing + 78-step BFS closure | ✅ |
| `geo_apex_wire.h` | 4-wire apex + coset routing | ✅ NEW |
| `geo_giant_array.h` | 12 GiantCubes + flat 4608B pack | ✅ NEW |

Dependencies (upstream):
- `geo_primitives.h` — `_mix64`, `_rotl64`, `derive_next_core`
- `pogls_fibo_addr.h` — `fibo_addr()`, PHI_UP/DOWN, FiboTwin
- `geo_config.h` — shared constants
- `geo_apex_activation.h` (Phase 3) — ApexRef, APEX_DORMANT/ACTIVE

---

## Apex Wire (4-wire, symmetry preserved)

```c
ApexWire {
    hilbert_a    // World A — PHI_UP addressing
    hilbert_b    // World B — PHI_DOWN addressing
    route        // base3: 0=pair 1=branch 2=skip
    reserved     // hook: echo/audit/loopback
}
```

Apex does NOT fan-out directly.
Symmetry preserved via LetterCube junction:
```
ApexWire → lc_route_coset() → GiantCube[0..11]
```

---

## LetterCube Role

- NOT storage — it is a **junction / routing proof**
- 6! = 720 permutations → BFS always finds island of 720
- 78-step closure (LCM(13,6)) = proof that routing never loops
- Coset = `(hilbert_a XOR hilbert_b XOR route_bias) % 720 / 60`

---

## GiantArray Layout

```
GiantArray[12 cosets]
  └── GiantCube[coset]
        └── FrustumSlot64[6 faces] × 64B
              └── core[4], addr[4], world[4]  ← L0..L3
```

Flat pack for GPU DMA:
```
[coset0: f0..f5 × 64B][coset1: ...] ... [coset11: ...]
= 12 × 6 × 64 = 4,608B
```

GPU kernel target:
```
gridDim.x  = 12   (one block per coset)
blockDim.x = 32   (one warp)
→ 144 threads = F(12) = 1 fibo clock
```

---

## Key Design Decisions (Locked)

| Decision | Value |
|----------|-------|
| `ApexRef.parent_core` | field name (not `master_core`) |
| `lock_core` formula | `pos.parent_core XOR rotl64(neg.parent_core, 32)` |
| `GpuFrustumSlot` size | 16B → 1 thread 1 slot |
| `FrustumSlot64` size | 64B → 1 cache line |
| Neighbor handshake | `apex_core` (no pointer) |
| Sacred numbers | 18 / 54 / 162 / 972 (all digit-sum 9) |
| Fibo clock | 144 = F(12) |
| Coset count | 12 = 720 / 60 |

---

## What's Next (Phase 4 → Complete)

| Task | File | Priority |
|------|------|----------|
| Serialize BigCube → binary + reconstruct | `geo_cube_file_store.h` | P1 |
| CUDA kernel: CubeGpuPayload dispatch | `geo_kernel.cu` | P1 |
| `gridDim=12 blockDim=32` → 144 threads | inside kernel | P1 |
| Phase 8 image layer (pyramid residual) | `geo_pyramid.h` | P2 |
| Streaming protocol | `geo_stream.h` | P2 |

---

## Phase 5+ Roadmap

```
Phase 3 (current) — Frustum / Cube / LetterCube / GiantArray
Phase 4            — File store + reconstruct (deterministic)
Phase 5            — CUDA kernel + GPU pipeline
Phase 6            — Temporal System (Fibbo Clock) Goal: ใช้ fibbo เป็น “เวลา”
ต้อง prove step index → map → fibbo stable
ไม่ overflow / loop ผิด
replay จาก step N → M ได้ตรง
test jump timeline (skip step)
rewind แบบ partial
Phase 7            —Error System (กันพังจริง) Goal: พลาดต้อง “รู้ทันที”
ต้องมี boundary validation (input gate)
step invariant check
checksum / hash per segment
design choice hard fail ❗ (แนะนำ)
auto-correct (ไว้ทีหลัง)
Phase 8            — Image/Video pyramid (direction-mapped)
```

---

*Locked by: Claude Sonnet 4.6 | Session: Phase 4 completion*
*Do not change dispatch math — 144 = F(12) is load-bearing.*
