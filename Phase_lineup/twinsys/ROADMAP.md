# POGLS / FGLS / LC — Roadmap & Proof Requirements

---

## Core Thesis (สิ่งที่ต้องพิสูจน์ได้ทั้งหมด)

> **Geometric address ที่ compute จาก data เองโดยไม่มี index กลาง สามารถทำ write/read/delete ได้เร็วกว่าหรือเทียบเท่า hash-based system บน append-only workload**

ถ้าพิสูจน์ได้ข้อนี้ข้อเดียว ทุกอย่างที่เหลือเป็น feature บนของที่ทำงานได้แล้ว

---

## Phase 0 — Minimum Viable Proof (ทำในสภาพแวดล้อมจำกัดได้)

**เป้าหมาย:** `write(addr, value)` → `read(addr)` กลับได้ถูกต้อง พร้อม benchmark ตัวเลข

**งานที่ต้องทำ:**

1. `geo_core_bench.c` — file เดียว ไม่มี dependency นอก libc
   - implement `trit_map(addr, value)` → `(trit, spoke, offset)`
   - implement `dodeca_insert(trit, spoke, offset, value)` → flat array 27 slots
   - implement `dodeca_read(trit, spoke)` → value
   - main loop: write 100,000 entries, read back ทั้งหมด, วัด ns/op

2. compile: `gcc -O2 -o bench geo_core_bench.c && ./bench`

**ตัวเลขที่ต้องวัด:**
- write throughput (ops/sec)
- read latency (ns/op)
- collision rate (กี่ % ของ write ที่ GROUND)
- memory footprint (bytes per entry)

**เปรียบเทียบกับ:** `std::unordered_map` หรือ simple hash table ใน C — ถ้าแพ้ทุกด้านต้องรู้ก่อนไปต่อ

**Definition of done:** ตัวเลขออกมาได้จริง ไม่ว่าจะดีหรือแย่

---

## Phase 1 — Fix Known Bugs (ก่อน benchmark จริงไม่มีความหมาย)

**P1a — GROUND path trit mapping** (bug ที่รู้อยู่แล้ว)
```c
// แก้ใน pogls_twin_bridge.h GROUND branch:
uint32_t trit  = (uint32_t)((addr ^ value) % 27u);
uint8_t  spoke = (uint8_t)(trit % 6u);
uint8_t  off   = (uint8_t)(trit % 9u);
dodeca_insert(&b->dodeca, (uint64_t)trit, spoke, 0, off, 0, 0);
```
[Change] raw mask → trit decomposition
[Why] raw mask ไม่ map เข้า dodeca territory อย่างถูกต้อง geometric invariant ไม่ hold
[Cost] 3 lines
[Risk] ต่ำ — test ด้วย existing `test_twin_bridge.c`
[Fallback] revert 3 lines

**P1b — `test_twin_bridge.c` crash** (null `pw.bundle` — จาก memory ก่อนหน้า)
ต้องแก้ก่อน P1a มีความหมาย — confirm `pw.bundle` non-NULL ที่ call site

---

## Phase 2 — Storage Layer (FGLS bridge)

**เป้าหมาย:** dodeca output → GiantArray → CubeFileStore (4,896B file on disk)

**งานที่ต้องทำ:**

1. `fgls_twin_store.h` — bridge header
   - `dodeca_to_giant(dodeca_out, trit)` → `GiantArray.cubes[trit/6].faces[trit%6]`
   - `giant_to_cfs(ga, reserved_mask)` → `CubeFileStore` (gcfs_serialize)
   - `cfs_write_file(cfs, path)` → 4,896B file

2. `twin_bridge_delete(b, addr)`:
   - compute trit จาก addr
   - `coset = trit / 6`
   - set bit ใน `cfs.hdr.reserved_mask`
   - `lc_twin_gate_filter(&b->lc_gate, face, angle)` — future write → GROUND_ABSORB

**ตัวเลขที่ต้องวัด:**
- file write time (ms per 4,896B chunk)
- delete + verify inaccessible (read after delete ต้องไม่คืน data)
- reconstruct from seed (deterministic mode ต้องได้ผลเดิม)

---

## Phase 3 — GeoPixel Encode/Decode

**เป้าหมาย:** `encode(data_block)` → PNG, `decode(PNG)` → data_block roundtrip ถูกต้อง 100%

**งานที่ต้องทำ:**

1. `geo_pixel.h` — C header-only
   - `geo_pixel_encode(idx, W)` → `{r, g, b}` ตาม formula:
     ```c
     R = ((idx%27) << 3) | (idx%6)
     G = ((idx%9) << 4) | (idx%26 & 0xF)
     B = idx % 144
     ```
   - `geo_pixel_decode(r, g, b)` → reconstruct fields (ไม่คืน idx ต้อง brute-force ใน range)
   - `geo_pixel_roundtrip_verify(W, H)` → 0 = no loss

2. decode gap: ตอนนี้ decode ยังไม่มี — ต้อง implement และพิสูจน์ว่า lossless ใน range ที่ระบุ

**สิ่งที่ต้องพิสูจน์:**
- roundtrip: encode → decode → same fields (ไม่ใช่ same idx เพราะ mod ไม่ invertible)
- uniqueness: ใน W×H grid ที่ W=27 ไม่มี pixel สองตัวที่มี (trit,spoke,coset,letter,fibo) เหมือนกันทุก field

---

## Phase 4 — End-to-End Demo (พร้อมคุยกับคนอื่น)

**เป้าหมาย:** demo ที่รันได้ใน Docker ใน 1 command แสดงผล benchmark จริง

```
docker run pogls-demo
→ write 1M entries
→ read 1M entries
→ delete 10K entries, verify inaccessible
→ print: throughput / latency / memory / collision_rate
→ compare: vs sqlite, vs redis (simple SET/GET)
```

**สิ่งที่ต้องพิสูจน์ใน demo:**
- write throughput ≥ 500K ops/sec (ถ้าต่ำกว่านี้ต้องอธิบายได้ว่าทำไม)
- read latency ≤ 500ns/op average
- delete ทำงานจริง (reserved_mask หรือ LC ghost)
- memory per entry ≤ hash table ปกติ

---

## ลำดับที่แนะนำ (เรียงตาม ROI ต่อ effort)

```
1. geo_core_bench.c    ← 1 session, 1 file, ได้ตัวเลขจริง
2. P1b test crash fix  ← unblock ทุกอย่างที่เหลือ
3. P1a GROUND trit fix ← 3 lines
4. fgls_twin_store.h   ← Phase 2 storage
5. geo_pixel roundtrip ← Phase 3
6. Docker demo         ← Phase 4
```

---

## Sacred Numbers (ห้ามเปลี่ยน — load-bearing constants)

| Value | Role |
|-------|------|
| 27 = 3³ | trit cycle, shared invariant ทุก layer |
| 6 | GEO_SPOKES = frustum faces = dodeca directions |
| 9 = 3² | GCFS_ACTIVE_COSETS |
| 144 = F(12) | fibo clock cycle |
| 54 = 2×3³ | icosphere faces |
| 720 = 6! = 144×5 | full dodeca rotation = 5 fibo cycles |
| 28,080 = LCM(27,26,144,720) | full address cycle (168×168 grid) |

---

## Context Budget (แก้ปัญหา token ล้น)

แต่ละ session ควร load แค่:
- handoff doc (SESSION_HANDOFF.md) — 1 file
- ไฟล์ที่จะแก้ใน session นั้น — max 2-3 files
- ไม่ load ทุก header พร้อมกัน

session เดียว = task เดียวจาก roadmap นี้ — ไม่ข้าม phase
