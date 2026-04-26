# POGLS Phase 9b — Session Handoff
> Historical checkpoint. This documents the phase 9b session state; prefer `phase11/` for the latest combined snapshot and see `PHASE_CONTINUITY.md` for the repo map.
**Date:** 2026-04-19  
**File:** `phase9b_v1.zip`  
**Status:** 22/22 tests PASS ✅

---

## What Was Built

### geo_fec.h — XOR Mini-Block FEC (stride-3 A+B)

FEC layer on top of TRing Phase 8 stream.  
No malloc. No float. Header-only.

**Layout:**
```
720 data slots
└─ 5 levels × 144 = GEO_PYR_PHASE_LEN
   └─ 12 blocks × 12 chunks = 144
      └─ 3 parity chunks per block (P0, P1, P2)

Total parity: 60 blocks × 3 = 180 chunks (25% overhead)
```

**Parity scheme (per block, indices 0..11):**
```
P0 (parity_idx=0): XOR(all 12)          — recovers any 1 loss
P1 (parity_idx=1): XOR(0, 3, 6, 9)     — stride-3 phase-0
P2 (parity_idx=2): XOR(1, 4, 7, 10)    — stride-3 phase-1
implicit phase-2 = {2, 5, 8, 11}        — covered by P0 XOR P1 XOR P2
```

**Recovery logic:**
- 1 loss → P0 XOR residual → direct recover
- 2 loss (mi, mj) → find parity Pk where Pk[mi] ≠ Pk[mj] → isolate one → P0 for the other
- 2 loss, all Pk[mi]==Pk[mj] → unrecoverable (graceful fail, no corruption)
- ≥3 loss → unrecoverable (beyond 2-parity scope, reserved for Phase 9c RS)

**2-loss coverage:**
```
48/66 pairs recoverable = 72%
18/66 unrecoverable = same-stride pairs:
  {0,3,6,9} C2 = 6  (both in P1)
  {1,4,7,10} C2 = 6  (both in P2)
  {2,5,8,11} C2 = 6  (in neither P1 nor P2)
```

**Key struct:**
```c
typedef struct {
    uint8_t  data[4096];
    uint16_t chunk_sizes[12];  // exact per-chunk size — critical for partial last chunk
    uint8_t  fec_type;         // 0=DATA 1=XOR 2=RS(reserved)
    uint8_t  fec_n;            // 3
    uint8_t  level;            // 0-4
    uint8_t  block;            // 0-11
    uint8_t  parity_idx;       // 0/1/2
    uint8_t  _pad[3];
} FECParity;
```

**parity[] layout:**  
`parity[block_global * 3 + parity_idx]`  
where `block_global = level * 12 + block` → index 0..59

**API:**
```c
fec_encode_all(store, parity)          // encode 720 chunks → 180 parity
fec_recover_all(r, store, parity)      // recover, returns chunks_recovered (uint16_t)
fec_gap_map(r, n_pkts, gap_map)        // gap bitmap + count
fec_encode_block(store, l, b, &par[0]) // per-block if needed
fec_recover_block(r, store, &par[0])   // returns chunks recovered (0/1/2)
```

---

## Test Results (test_fec.c — 22/22)

| Test | Description | Result |
|------|-------------|--------|
| T1 | No loss | PASS — 0 recovered, store intact |
| T2 | 1 loss/block × 60 | PASS — 60/60 recovered, byte-perfect |
| T3 | 2 loss/block (0,1) × 60 — recoverable | PASS — 120/120, byte-perfect |
| T4 | 2 loss/block (2,5) × 60 — unrecoverable | PASS — 0 recovered, no corruption |
| T5 | Random 20% scatter (seed 0xDEADBEEF) | PASS — 51/144 recovered |
| T6 | Exhaustive 66 pairs block 0 | PASS — 48/66 actual = 48/66 theory, 0 corrupt |

**T5 detail:** 144 dropped → 51 recovered → 93 remaining gaps  
vs Phase 9a (1 parity): only 11/144 recovered with same seed

---

## Bugs Found & Fixed This Session

**Bug 1 (Phase 9a → 9b):** `FECParity.size` stored `max_size` of block, not per-chunk size.  
Recovered partial last chunk got `size=4096` instead of original `size=1337`.  
**Fix:** Added `chunk_sizes[12]` — encode stores exact size per slot, recover uses `chunk_sizes[missing_idx]`.

**Bug 2 (test only):** T3 used drop pair (0,3) — both in S3A → same-stride, unrecoverable by design.  
**Fix:** Changed to (0,1) — S3A[0]=1, S3A[1]=0 → isolated correctly.

---

## Scheme Selection: Why stride-3 A+B

Tested 5 schemes × 10 random seeds at 20% loss:

| Scheme | Pairs covered | T4 avg | Parity |
|--------|--------------|--------|--------|
| odd/even | 54% | 31.7/144 | 2 |
| stride-3 A only | 48% | — | 2 |
| **stride-3 A+B** | **72%** | **37.3/144** | **3** |
| stride-4 A+B | 68% | 25.7/144 | 3 |
| even+odd (3par) | 54% | 39/144 | 3 |

stride-3 A+B wins: +5.6 chunks/run avg over odd/even, consistent across seeds.

---

## File Inventory (phase9b_v1.zip)

| File | Role |
|------|------|
| `geo_fec.h` | **NEW** — Phase 9b FEC, stride-3 A+B |
| `test_fec.c` | **NEW** — 22-test suite |
| `geo_tring_stream.h` | Phase 8 — segmented streaming |
| `geo_temporal_ring.h` | Phase 7 — TRing 720-slot |
| `geo_pyramid.h` | Phase 7 — 5-level pyramid |
| `geo_temporal_lut.h` | baked LUT (from bake_lut3) |
| `bake_lut3.c` | LUT generator |
| `test_tring.c` | Phase 7 unit (22 tests) |
| `test_pyramid.c` | Phase 7 pyramid (37 tests) |
| `test_tring_stream.c` | Phase 8 stream (41 tests) |
| `test_tring_stream_seg.c` | Phase 8 segmented (30 tests) |

**Total tests across all files: 22 (9b) + 130 (8) = 152 tests**

---

## Build Instructions (Colab)

```python
import zipfile, os, pathlib, subprocess

ZIP  = 'phase9b_v1.zip'
WORK = pathlib.Path('/tmp/p9b')
WORK.mkdir(exist_ok=True)

with zipfile.ZipFile(ZIP) as z:
    z.extractall(WORK)

def run(cmd):
    r = subprocess.run(cmd, shell=True, cwd=WORK, capture_output=True, text=True)
    if r.stdout: print(r.stdout)
    if r.stderr: print(r.stderr)
    assert r.returncode == 0
    return r

# 1. bake LUT
run('gcc -O2 -o bake_lut3 bake_lut3.c && ./bake_lut3 > geo_temporal_lut.h')

# 2. Phase 8 unit tests (130)
for src, bin_ in [
    ('test_tring.c','test_tring'),
    ('test_pyramid.c','test_pyramid'),
    ('test_tring_stream.c','test_tring_stream'),
    ('test_tring_stream_seg.c','test_tring_stream_seg'),
]:
    run(f'gcc -O2 -o {bin_} {src} && ./{bin_}')

# 3. Phase 9b FEC tests (22)
run('gcc -O2 -o test_fec test_fec.c && ./test_fec')

print('All 152 tests passed ✅')
```

---

## Phase 9c Roadmap (next session)

**Goal:** RS (Reed-Solomon) backend swap — same struct layout, swap encode/decode only.

**What changes:**
- `fec_type = FEC_TYPE_RS` (already defined, value=2)
- `fec_encode_block` → RS systematic encoder over GF(256)
- `fec_recover_block` → Berlekamp-Massey or direct Vandermonde solve
- `FECParity.data` already sized for RS parity output

**What stays identical:**
- `FECParity` struct layout
- `fec_encode_all` / `fec_recover_all` call signatures
- Block addressing: `parity[block_global * FEC_PARITY_PER_BLOCK + idx]`
- All test scaffolding in `test_fec.c`
- `fec_n` field = number of parity per block (increase freely)

**RS benefit:** `fec_n=k` parity → tolerate any `k` losses per block (not just recoverable pairs)  
With `fec_n=3` → tolerate 3 losses per block → T4 20% loss → ~100% recovery

**GF(256) primitive poly:** 0x11D (x⁸+x⁴+x³+x²+1) — standard RS poly

**First step:** implement `gf256_mul`, `gf256_pow`, build Vandermonde matrix for 12-chunk blocks.
