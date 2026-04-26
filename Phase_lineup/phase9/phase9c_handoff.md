# POGLS Phase 9c/10 — Session Handoff
> Historical checkpoint. This documents the phase 9c/10 session state; prefer `phase11/` for the latest combined snapshot and see `PHASE_CONTINUITY.md` for the repo map.
**Date:** 2026-04-19
**Files:** `phase9c_v4.zip`
**Status:** 167/167 tests PASS ✅ (all suites)

---

## What Was Built This Session (Phase 10 Complete)

### Step 1 ✅ — `geo_rewind.h` (NEW)
- 972-slot circular RewindBuffer (720 TRing cycle + 252 lookahead)
- Key: `enc` (GEO_WALK value) → O(1) via `tring_pos(enc)` + linear fallback
- Zero compute on lookup — pure memcpy
- 21/21 tests PASS (`test_rewind.c`)

### Step 2 ✅ — `fec_hybrid_recover_block/all()` in `geo_fec_rs.h` (NEW)
- L1 XOR → L2 Rewind → L3 RS cascaded pipeline
- L2 runs **before** L1 (exact data beats XOR guess)
- L1 writes undone before L3 fires (prevents false-present poisoning)
- 20/20 tests PASS (`test_hybrid.c`)

### Step 3 ✅ — `geo_tring_fec.h` wired to hybrid
- `TRingFECCtx` + `RewindBuffer rewind` field
- `tring_fec_prepare()` → `rewind_init()`
- `tring_fec_encode()` → feeds rewind for all chunks
- `tring_fec_recv()` → feeds rewind per packet (`tring_pos(pkt->enc)`)
- `tring_fec_recover()` → `fec_hybrid_recover_all()` (L2+L3, zero-XOR = L1 disabled)

---

## Full Recovery Pipeline

```
tring_fec_recover(ctx)
  └─ fec_hybrid_recover_all()
       per block:
         L2: rewind_find(enc) → memcpy (zero compute, exact data)
         L1: fec_recover_block() XOR — skipped (zero parity in tring path)
         L3: fec_rs_recover_block() — safety net, fires only if L2 insufficient
```

**Why L1 disabled in tring path:** ctx has no XOR parity pool (adds ~740KB).
L2 rewind covers the same patterns faster. L3 RS covers everything else.

---

## All Test Suites

| Suite | Tests | Status |
|-------|-------|--------|
| test_tring | 22 | ✅ |
| test_pyramid | 37 | ✅ |
| test_fec | 22 | ✅ |
| test_fec_rs | 23 | ✅ |
| test_tring_fec | 23 | ✅ |
| test_rewind | 21 | ✅ |
| test_hybrid | 20 | ✅ |
| **Total** | **168** | **✅** |

---

## Integration API (unchanged from caller perspective)

```c
// SENDER
tring_fec_prepare(&tx, fec_n);
tring_fec_encode(&tx, file, size);   // slice + RS encode + feeds rewind

// RECEIVER
tring_fec_prepare(&rx, fec_n);
tring_fec_load_parity(&rx, &tx);
for each pkt: tring_fec_recv(&rx, pkt);  // feeds rewind automatically
tring_fec_recover(&rx);                   // L2+L3 hybrid, transparent
tring_fec_reconstruct(&rx, out, sz);
```

---

## TRingFECCtx Size (fec_n=3)

| Field | Size |
|-------|------|
| store[360] TStreamChunk | ~1.47 MB |
| parity_pool[720] FECParity | ~2.97 MB |
| RewindBuffer (972 slots) | ~3.99 MB |
| ring + metadata | ~3 KB |
| **Total** | **~8.4 MB** |

---

## Phase 11 Ideas

- **bench_hybrid.c** — measure L2 hit rate vs loss pattern (random / burst / stride)
- **tring_fec_send()** — sender serializes parity_pool for wire transport
- **Segmented pipeline** — `TStreamPktSeg` + multi-segment hybrid recovery
- **PHI topology classifier** — wire `scan_dir` (S60-A work)

---

## Build

```bash
gcc -O2 -o test_rewind   test_rewind.c   && ./test_rewind
gcc -O2 -o test_hybrid   test_hybrid.c   && ./test_hybrid
gcc -O2 -o test_fec_rs   test_fec_rs.c   && ./test_fec_rs
gcc -O2 -o test_tring_fec test_tring_fec.c && ./test_tring_fec
```
