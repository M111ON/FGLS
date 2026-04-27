# GeoPixel × POGLS Integration — Session Handoff
> Claude Sonnet 4.6 | Session boundary handoff

---

## Session Summary

ค้นพบ **GeoPixel encoding concept** — pixel coordinate (x,y) → POGLS geometric address โดยตรง
และพัฒนา `hilbert_vault_v4.py` (SVG QR-pattern + gzip) จาก v3

---

## Files Produced This Session

```
hilbert_vault_v4.py   ← encode folder → .vault.svgz (25% smaller than v3)
geo_pixel_demo.html   ← interactive visualizer: (x,y) → trit/spoke/letter/fibo
```

---

## GeoPixel Concept (Core Insight)

### Coordinate Transform Formula
```
idx   = y × W + x          ← raster order (top-left → right → down)

trit  = idx % 27           ← 3^3 ternary address
spoke = idx % 6            ← dodeca direction (GEO_SPOKES)
coset = idx % 9            ← GCFS_ACTIVE_COSETS
letter= idx % 26           ← LC pair A-Z
fibo  = idx % 144          ← fibo clock gear
```

### RGB Encoding (Primary Colors Only — confirmed direction)
```
R = trit   (0-26)   ← ternary position
G = spoke  (0-5)    ← dodeca direction / next hop
B = letter (0-51)   ← LC pair index (upper+lower = 52)
```

**Key decision:** ใช้แค่แม่สี RGB 3 channel — ไม่ต้องใช้ full 8bit
space ที่เหลือใน each channel = room for error correction / rotation alignment อนาคต

### Reading Order
- **เสมอ: top-left → right → next row → bottom-right**
- fix ambiguity ทั้งหมด — deterministic traversal
- direction ใน pixel = semantic next hop (ไม่ใช่ raster next)

### Why This Is Not Normal Encoding
```
ปกติ: pixel = data storage
GeoPixel: pixel = seed descriptor → reconstruct data จาก geometric rules
```
- screenshot ได้ก็ decode ได้ (ข้อมูลอยู่ใน visible pixel จริงๆ)
- QR code ธรรมดาทำไม่ได้

---

## Error Correction Path (ยังไม่ทำ — อนาคต)

**Rotation** จาก POGLS V1 (function เก็บไว้ไม่เคยใช้) คือ error correction โดยธรรมชาติ:
```
pixel เบี้ยว/หาย → rotate frame → หา canonical alignment
จนกว่า structure ลงตัวกับ 27/54/144 invariant
= zero overhead (ใช้ structure เดิม ไม่ต้องเพิ่ม redundant bytes)
```
เทียบ Reed-Solomon = algebraic + overhead
Rotation = geometric + zero overhead

---

## hilbert_vault_v4.py Changes vs v3

| | v3 | v4 |
|--|--|--|
| container | plain .svg | gzip .svgz |
| visual layer | text label | 16×16 PNG QR (folder SHA256) |
| QR source | — | sha256 bytes → 1-bit PNG data URI |
| size (13 files / 114KB) | 45,998B | 34,269B (-25%) |
| decode compat | v3 only | auto-detect .svg/.svgz |
| dead code | decode_unused() ✗ | removed ✓ |

**Why QR = SHA not data:** data อยู่ใน `<vault:file>` metadata แล้ว
QR คือ identity fingerprint เท่านั้น — scan ได้, unique per folder

---

## POGLS Main Tasks (จาก previous session — ยังค้างอยู่)

### P0 — Fix GROUND path (trit mapping) ← NEXT PRIORITY
แก้ใน `pogls_twin_bridge.h` GROUND branch:
```c
// CURRENT (naive — wrong):
uint64_t ground_addr = raw & UINT64_C(0x00FFFFFFFFFFFFFF);
dodeca_insert(&b->dodeca, ground_addr, 0, 0, offset, 0, 0);

// FIX:
uint32_t trit  = (uint32_t)((addr ^ value) % 27u);
uint8_t  spoke = (uint8_t)(trit % 6u);
uint8_t  off   = (uint8_t)(trit % 9u);
dodeca_insert(&b->dodeca, (uint64_t)trit, spoke, 0, off, 0, 0);
```

### P1 — `fgls_twin_store.h` (NEW)
Bridge: `dodeca_insert` output → `GiantArray`:
- coset = `trit / 6` (0-3 of 9 active)
- face  = `trit % 6` (0-5 = 6 frustum directions)
- serialize → `CubeFileStore` (4,896B)

### P2 — Delete API
```c
void twin_bridge_delete(TwinBridge *b, uint64_t addr);
// → trit → coset → set reserved_mask bit ใน CubeFileStore header
```

### P3 — LetterCube coupling
`lc_pair_match()` feed จาก `fibo_clock` seed เป็น `slope_hash`
→ LetterPair index = `addr % 26`

---

## Files Available (decoded from vault this session)

```
lc_gcfs_pkg_decoded/
├── src/lc_hdr.h          ← LCHdr pack/unpack, palette, lch_gate
├── src/lc_wire.h         ← lcw_space, lcw_route, LCGate enum
├── src/lc_fs.h           ← GCFS chunk (4,896B), lc_fs_write
├── src/lc_gcfs_wire.h    ← GCFS wire protocol
├── src/lc_delete.h       ← ghost node, triple-cut mechanism
├── src/lc_api.c          ← C API
├── lc_gcfs/lc_client.py  ← Python client
└── lc_gcfs/server.py     ← server
```
**lc_hdr.h + lc_wire.h** — unblocks P0/P1 (no longer missing)

---

## Sacred Numbers (DO NOT CHANGE)

| Value | Source | Role in GeoPixel |
|-------|--------|-----------------|
| 27 | 3^3 | trit cycle per pixel |
| 6 | GEO_SPOKES | spoke / direction |
| 9 | 3^2 | coset |
| 26 | A-Z | letter pair |
| 52 | 26×2 | upper+lower letter space → B channel |
| 144 | F(12) | fibo clock |

---

## Next Session Priority Order

1. **P0** — Fix GROUND path trit mapping ใน `pogls_twin_bridge.h`
2. **GeoPixel step 1** — implement encode (RGB from trit/spoke/letter) → PNG output
3. **P1** — `fgls_twin_store.h`
4. **GeoPixel step 2** — decode + roundtrip verify
