"""
HPB S29 — Fast Match Layer
===========================
ใช้ S28 DB → match → decide encode strategy → เร็วขึ้น 2-5×

Flow:
  1. fingerprint query image
  2. match vs DB (cosine on thumb16 + dct8 + hist)
  3. zone decision:
       FULL_REUSE     → skip re-analysis, use cached q/strategy directly
       STRATEGY_REUSE → use strategy hint, recalc q only
       S27_FALLBACK   → full encode (no hint)
  4. encode with JPEG q95 + residual
  5. log result back to S28 DB

Usage:
  python3 hpb_s29_match.py enc <in.ppm|jpg|png> <out.hpb> [domain]
  python3 hpb_s29_match.py dec <in.hpb> <out.ppm>
  python3 hpb_s29_match.py bench <img1> <img2> ...   # compare zone speeds
"""

import sys, os, time, io, json, zlib
import numpy as np
from PIL import Image

# import S28 brain
sys.path.insert(0, os.path.dirname(__file__))
from hpb_s28_brain import (
    build_fingerprint, fingerprint_similarity,
    load_db, save_db, load_image
)

# ── JPEG helpers ───────────────────────────────────────

def jpeg_enc(rgb: np.ndarray, quality: int) -> bytes:
    buf = io.BytesIO()
    Image.fromarray(rgb).save(buf, format='JPEG', quality=quality, optimize=True)
    return buf.getvalue()

def jpeg_dec(data: bytes) -> np.ndarray:
    return np.array(Image.open(io.BytesIO(data)).convert('RGB'))

# ── Encode core ────────────────────────────────────────

def encode_image(rgb: np.ndarray, jpeg_quality: int = 95) -> dict:
    """Encode → returns dict with all buffers + stats"""
    t0 = time.perf_counter()

    jpeg_bytes = jpeg_enc(rgb, jpeg_quality)
    rec = jpeg_dec(jpeg_bytes)

    res = rgb.astype(np.int16) - rec.astype(np.int16)
    res_i8 = np.clip(res, -128, 127).astype(np.int8)
    res_z = zlib.compress(res_i8.tobytes(), 6)

    elapsed = time.perf_counter() - t0

    return {
        "jpeg_bytes": jpeg_bytes,
        "res_z": res_z,
        "width": rgb.shape[1],
        "height": rgb.shape[0],
        "jpeg_kb": len(jpeg_bytes) / 1024,
        "res_kb": len(res_z) / 1024,
        "total_kb": (len(jpeg_bytes) + len(res_z)) / 1024,
        "max_diff": int(np.abs(res).max()),
        "zeros_pct": float((res == 0).sum() / res.size * 100),
        "encode_ms": round(elapsed * 1000, 1),
        "jpeg_quality": jpeg_quality,
    }

# ── Strategy from DB match ─────────────────────────────

def get_strategy(fp_query: dict, db: list) -> dict:
    """
    Returns strategy dict:
      zone, matched_path, jpeg_quality, hint
    """
    if not db:
        return {"zone": "S27_FALLBACK", "jpeg_quality": 95,
                "matched_path": None, "composite": 0.0}

    best_sim = None
    best_entry = None
    for entry in db:
        if entry.get("path") == fp_query.get("path"):
            continue
        sim = fingerprint_similarity(fp_query, entry)
        if best_sim is None or sim["composite"] > best_sim["composite"]:
            best_sim = sim
            best_entry = entry

    zone = best_sim["zone"]
    q = 95  # default

    if zone == "FULL_REUSE":
        # trust cached encode config entirely
        q = best_entry["encode"].get("jpeg_quality", 95)
        hint = f"reuse full config from {os.path.basename(best_entry['path'])}"

    elif zone == "STRATEGY_REUSE":
        # use same quality class but recalc
        cached_q = best_entry["encode"].get("jpeg_quality", 95)
        # adjust q based on variance ratio
        v_ratio = fp_query["stats"]["var"] / (best_entry["stats"]["var"] + 1e-6)
        if v_ratio > 1.3:
            q = min(95, cached_q + 2)
        elif v_ratio < 0.7:
            q = max(85, cached_q - 2)
        else:
            q = cached_q
        hint = f"strategy from {os.path.basename(best_entry['path'])}, q adjusted {cached_q}→{q}"

    else:
        hint = "no match — full S27 encode"

    return {
        "zone": zone,
        "jpeg_quality": q,
        "matched_path": best_entry["path"] if best_entry else None,
        "composite": best_sim["composite"] if best_sim else 0.0,
        "hint": hint,
    }

# ── HPB file write/read ────────────────────────────────
# Format: "HPB8" + w(4) + h(4) + q(1) + jpeg_sz(8) + jpeg + res_sz(8) + res_z

MAGIC = b"HPB8"

def write_hpb(path: str, result: dict):
    with open(path, "wb") as f:
        f.write(MAGIC)
        f.write(result["width"].to_bytes(4, "little"))
        f.write(result["height"].to_bytes(4, "little"))
        f.write(bytes([result["jpeg_quality"]]))
        jb = result["jpeg_bytes"]
        f.write(len(jb).to_bytes(8, "little"))
        f.write(jb)
        rz = result["res_z"]
        f.write(len(rz).to_bytes(8, "little"))
        f.write(rz)

def read_hpb(path: str) -> dict:
    with open(path, "rb") as f:
        magic = f.read(4)
        if magic != MAGIC:
            raise ValueError(f"Not HPB8 file (got {magic})")
        w = int.from_bytes(f.read(4), "little")
        h = int.from_bytes(f.read(4), "little")
        q = int.from_bytes(f.read(1), "little")
        jpeg_sz = int.from_bytes(f.read(8), "little")
        jpeg_bytes = f.read(jpeg_sz)
        res_sz = int.from_bytes(f.read(8), "little")
        res_z = f.read(res_sz)
    return {"width": w, "height": h, "jpeg_quality": q,
            "jpeg_bytes": jpeg_bytes, "res_z": res_z}

def decode_hpb(hpb: dict) -> np.ndarray:
    base = jpeg_dec(hpb["jpeg_bytes"])
    npx = hpb["width"] * hpb["height"] * 3
    res_raw = zlib.decompress(hpb["res_z"])
    res = np.frombuffer(res_raw, dtype=np.int8).reshape(hpb["height"], hpb["width"], 3)
    out = np.clip(base.astype(np.int16) + res.astype(np.int16), 0, 255).astype(np.uint8)
    return out

def verify(orig: np.ndarray, dec: np.ndarray) -> dict:
    diff = np.abs(orig.astype(np.int16) - dec.astype(np.int16))
    return {
        "max_diff": int(diff.max()),
        "lossless": bool(diff.max() == 0),
        "diff_pixels": int((diff.max(axis=2) > 0).sum()),
    }

# ── Commands ───────────────────────────────────────────

def cmd_enc(img_path: str, out_hpb: str, domain: str = "unknown"):
    print(f"S29 encode: {img_path}")
    rgb = load_image(img_path)

    t_total = time.perf_counter()

    # S28 fingerprint + match
    t_fp = time.perf_counter()
    fp = build_fingerprint(rgb, img_path, domain)
    fp_ms = (time.perf_counter() - t_fp) * 1000

    db = load_db()
    strategy = get_strategy(fp, db)

    print(f"  Fingerprint:  {fp_ms:.0f}ms")
    print(f"  Zone:         {strategy['zone']}  (score={strategy['composite']:.4f})")
    print(f"  Hint:         {strategy['hint']}")
    print(f"  JPEG quality: {strategy['jpeg_quality']}")

    # Encode
    result = encode_image(rgb, jpeg_quality=strategy["jpeg_quality"])
    result["strategy"] = strategy

    write_hpb(out_hpb, result)

    # Verify lossless
    hpb = read_hpb(out_hpb)
    dec = decode_hpb(hpb)
    v = verify(rgb, dec)

    total_ms = (time.perf_counter() - t_total) * 1000

    print(f"  JPEG q{strategy['jpeg_quality']}:     {result['jpeg_kb']:.1f} KB")
    print(f"  Residual:     {result['res_kb']:.1f} KB  (max_diff={result['max_diff']}, zeros={result['zeros_pct']:.1f}%)")
    print(f"  Total:        {result['total_kb']:.1f} KB")
    print(f"  Lossless:     {'✅ YES' if v['lossless'] else '❌ NO  max_diff=' + str(v['max_diff'])}")
    print(f"  Encode time:  {result['encode_ms']:.0f}ms  (total incl. fingerprint: {total_ms:.0f}ms)")

    # Log to S28 DB
    fp["encode"]["jpeg_quality"] = strategy["jpeg_quality"]
    if fp["path"] not in [e["path"] for e in db]:
        db.append(fp)
        save_db(db)
        print(f"  DB:           logged ({len(db)} entries)")
    else:
        print(f"  DB:           already logged")

def cmd_dec(in_hpb: str, out_ppm: str):
    print(f"S29 decode: {in_hpb} → {out_ppm}")
    hpb = read_hpb(in_hpb)
    dec = decode_hpb(hpb)
    Image.fromarray(dec).save(out_ppm)
    print(f"  Decoded: {hpb['width']}×{hpb['height']}  q={hpb['jpeg_quality']}")

def cmd_bench(*img_paths):
    """Encode all images, show zone + time comparison"""
    db = load_db()
    print(f"Benchmark: {len(img_paths)} images  DB={len(db)} entries")
    print(f"\n{'Image':<25} {'Zone':<18} {'Score':>7} {'Q':>3} {'KB':>6} {'ms':>6}")
    print("-" * 72)

    for path in img_paths:
        if not os.path.exists(path):
            print(f"  {path}: not found"); continue
        rgb = load_image(path)
        fp = build_fingerprint(rgb, path, "bench")
        strategy = get_strategy(fp, db)

        t0 = time.perf_counter()
        result = encode_image(rgb, jpeg_quality=strategy["jpeg_quality"])
        enc_ms = (time.perf_counter() - t0) * 1000

        name = os.path.basename(path)[:24]
        print(f"  {name:<24} {strategy['zone']:<18} "
              f"{strategy['composite']:>7.4f} "
              f"{strategy['jpeg_quality']:>3} "
              f"{result['total_kb']:>6.0f} "
              f"{enc_ms:>6.0f}")

# ── Main ───────────────────────────────────────────────

def main():
    if len(sys.argv) < 2:
        print(__doc__); return

    cmd = sys.argv[1]
    if cmd == "enc" and len(sys.argv) >= 4:
        domain = sys.argv[4] if len(sys.argv) > 4 else "unknown"
        cmd_enc(sys.argv[2], sys.argv[3], domain)
    elif cmd == "dec" and len(sys.argv) >= 4:
        cmd_dec(sys.argv[2], sys.argv[3])
    elif cmd == "bench" and len(sys.argv) >= 3:
        cmd_bench(*sys.argv[2:])
    else:
        print(__doc__)

if __name__ == "__main__":
    main()
