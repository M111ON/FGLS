# FGLS Phase 6–7 Handoff
> Historical checkpoint. Some files listed here are not present in `phase6/`; use the repo-root continuity docs before treating this as authoritative current state.
## Status: SEALED ✅

---

## Files Delivered

| File | Role | Test |
|------|------|------|
| `geo_fibo_temporal.h` | Hybrid snapshot+delta temporal system | 23/23 ✅ |
| `geo_temporal_lut.h` | AUTO-GEN — GEO_WALK[720]+GEO_PAIR[720]+IDX | bake verify ✅ |
| `geo_temporal_ring.h` | Runtime TRing API (pure lookup) | 11/11 ✅ |
| `geo_tring_stream.h` | File streaming layer on TRing | 13/13 ✅ |
| `bake_lut3.c` | LUT generator — run once at build time | — |

---

## Phase 6 — Fibo Temporal System

**Hybrid model:** FLUSH snapshot (every 144 steps) + delta log (fx values between flushes)

```
FiboTimeline
├── snap[256]     FiboSnapshot — full FiboCtx at every FLUSH boundary
├── delta         FiboDeltaLog — fx[144] between snapshots
├── ctx           live FiboCtx
└── step          uint64_t global counter

FLUSH = 144 steps = FIBO_PERIOD_FLUSH = TE_CYCLE  ← load-bearing
snapshot ring = 256 slots → covers 256×144 = 36,864 steps
```

**API — CRITICAL contract: `fx_seq` must always start from `snap->step`, not from N**

```c
fibo_timeline_init(tl, seed)
fibo_timeline_tick(tl, fx)            // auto-snap at FLUSH
fibo_timeline_replay(tl, N, M, fx*)   // fx* from snap_step
fibo_timeline_jump(tl, N, fx*, len)   // restore to step N
fibo_timeline_rewind(tl, k, fx*, n)   // rewind k steps
fibo_timeline_skip(tl, n)             // debug only — no fx
```

---

## Phase 7 — Geometric Temporal Ring

### Core Discovery: Position = Order = Time

```
dodecahedron 12 pentagon faces
→ 12 Compound-of-5-Tetrahedra (1 per face)
→ each compound: 5 tets × 4 verts × 3 rots = 60 unique tuples
→ 12 × 60 = 720 = FIBO_PERIOD_SNAP = 6! ✓

walk position = sequence order = time
no arithmetic at runtime — geometry IS the clock
```

### Tuple Encoding

```c
uint32: [comp:4][tet:3][vert:2][rot:2]  // 11 bits, 720 unique
TRING_COMP(e), TRING_TET(e), TRING_VERT(e), TRING_ROT(e)
TRING_CPAIR(e)  // comp → (11-comp), O(1), bijection
GEO_WALK_IDX[enc & 0x7FF]  // reverse lookup O(1)
```

### BFS Walk Order (LetterCube style)

```
seed: slot 0
neighbors: chiral partner (11-s) first → (s+1)%12
→ 720 unique by construction, no rotation matrix needed
pair involution: pair(pair(v)) == v ✓
```

### TRing Runtime API

```c
tring_init(r)
tring_tick(r)                    // head++ mod 720 = clock tick
tring_pos(enc)                   // enc → walk index O(1)
tring_pair_pos(enc)              // enc → chiral partner index O(1)
tring_snap(r, enc)               // snap head to arriving enc, returns gap
tring_first_gap(r)               // scan for missing slot
```

### Streaming Layer

```c
// Sender
tstream_slice_file(s, pkts, file, fsize)  // hashes file → N packets

// Receiver
tstream_recv_pkt(r, store, pkt)   // returns 0=ok, >0=gap, -1=bad enc, -2=CRC fail
tstream_reconstruct(r, store, n)  // walk order → output buffer

// Packet: [enc:4B][size:2B][crc16:2B][data:4096B]
// gap → zero-filled in output
```

---

## Sacred Numbers — DO NOT CHANGE

```
144  = FIBO_PERIOD_FLUSH = TE_CYCLE = F(12)  ← load-bearing
720  = FIBO_PERIOD_SNAP = 6! = TRing cycle
360  = 720/2 = chiral pair count
12   = N_COMP = dodecahedron faces
60   = tuples per compound = 5×4×3
```

---

## Key Invariants

```
1. fx_seq in replay/jump/rewind → always starts from snap->step
2. __noinline__ on _d_coset_checksum (from Phase 5, still required)
3. TRING_CPAIR: partner = (11 - comp) — chiral, not modular
4. bake_lut3.c must pass verify PASS before committing LUT
5. TRing head wraps at 720 → aligns with fibo SNAP boundary
```

---

## What Remains

| Task | Priority |
|------|----------|
| `geo_pyramid.h` — residual layers (Phase 8) | P1 |
| Full round-trip: serialize → TRing stream → reconstruct → kernel | P1 |
| Async multi-stream (dedicated GPU) | P2 |

---

## Two Open Bugs (geo_final_v1.h — pre-existing)

```
1. dodeca_score always returns 0  (self-mirror math on err9)
2. geonet_map_fast wrong Barrett constant for div6
```

---

*Sealed: Claude Sonnet 4.6 | FGLS Phase 6–7 Complete*
