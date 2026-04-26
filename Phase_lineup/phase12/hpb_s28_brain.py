"""
HPB S28 — Data Brain
====================
Log every encode → build fingerprint DB → "รู้ว่าอะไรคล้ายอะไร"

Fingerprint per image (stored in brain_db.json):
  - thumb16: 16×16 RGB downsample (768 floats, normalized 0-1)
  - dct8:    top-8 DCT coefficients per channel (captures frequency signature)
  - hist:    32-bin histogram per channel (96 floats)
  - stats:   variance, edge%, mean, width, height, domain
  - encode:  jpeg_kb, residual_kb, total_kb, max_diff, zeros_pct
  - meta:    path, timestamp, domain tag

Usage:
  python3 hpb_s28_brain.py log <image.ppm|jpg|png> [domain]
  python3 hpb_s28_brain.py match <image.ppm|jpg|png>
  python3 hpb_s28_brain.py stats
  python3 hpb_s28_brain.py show_similar <image> [top_n]
"""

import sys, os, json, time, math
import numpy as np
from PIL import Image
import io

DB_PATH = "brain_db.json"
THUMB_SIZE = 16
DCT_TOP = 8
HIST_BINS = 32

# ── Fingerprint extraction ─────────────────────────────

def extract_thumb(img_rgb: np.ndarray) -> list:
    """16×16 downsample, normalized 0-1"""
    pil = Image.fromarray(img_rgb).resize((THUMB_SIZE, THUMB_SIZE), Image.LANCZOS)
    arr = np.array(pil, dtype=np.float32) / 255.0
    return arr.flatten().tolist()

def extract_dct8(img_rgb: np.ndarray) -> list:
    """Top-8 DCT coefficients per channel from 32×32 center crop"""
    h, w = img_rgb.shape[:2]
    # center crop 32×32
    cy, cx = h // 2, w // 2
    crop = img_rgb[max(0,cy-16):cy+16, max(0,cx-16):cx+16]
    pil = Image.fromarray(crop).resize((32, 32), Image.LANCZOS)
    arr = np.array(pil, dtype=np.float32)

    result = []
    for c in range(3):
        ch = arr[:, :, c] - 128.0
        # 2D DCT via separable 1D DCT approximation (no scipy needed)
        # use numpy FFT-based approach
        N = 32
        # Type-II DCT via FFT
        x = ch.flatten()
        v = np.zeros(N * N)
        v[:N*N] = x
        # simple: just use top frequencies from abs(FFT)
        f = np.abs(np.fft.rfft2(ch))
        f_flat = f.flatten()
        idx = np.argsort(f_flat)[::-1]
        # skip DC (idx 0), take next DCT_TOP
        top = f_flat[idx[1:DCT_TOP+1]]
        # normalize by total energy
        total = f_flat.sum() + 1e-6
        result.extend((top / total).tolist())
    return result  # 24 values

def extract_hist(img_rgb: np.ndarray) -> list:
    """32-bin histogram per channel, normalized"""
    result = []
    for c in range(3):
        h, _ = np.histogram(img_rgb[:, :, c], bins=HIST_BINS, range=(0, 256))
        h = h.astype(np.float32) / (h.sum() + 1e-6)
        result.extend(h.tolist())
    return result  # 96 values

def extract_edge_pct(img_rgb: np.ndarray) -> float:
    """Simple Sobel edge percentage"""
    gray = img_rgb.mean(axis=2).astype(np.float32)
    gx = np.abs(np.diff(gray, axis=1))
    gy = np.abs(np.diff(gray, axis=0))
    edge_x = (gx > 20).sum()
    edge_y = (gy > 20).sum()
    total = gray.size
    return float((edge_x + edge_y) / (2 * total))

def extract_stats(img_rgb: np.ndarray) -> dict:
    h, w = img_rgb.shape[:2]
    return {
        "width": w,
        "height": h,
        "mean_rgb": img_rgb.mean(axis=(0,1)).tolist(),
        "var": float(img_rgb.astype(np.float32).var()),
        "edge_pct": extract_edge_pct(img_rgb),
    }

def encode_stats(img_rgb: np.ndarray) -> dict:
    """Simulate encode and collect residual stats"""
    buf = io.BytesIO()
    Image.fromarray(img_rgb).save(buf, format='JPEG', quality=95)
    jpeg_kb = len(buf.getvalue()) / 1024

    buf.seek(0)
    rec = np.array(Image.open(buf).convert('RGB'))
    res = img_rgb.astype(np.int16) - rec.astype(np.int16)

    zeros_pct = float((res == 0).sum() / res.size)
    max_diff = int(np.abs(res).max())

    # estimate residual size via zlib (proxy for zstd)
    import zlib
    raw = res.astype(np.int8).tobytes()
    compressed = zlib.compress(raw, 6)
    res_kb = len(compressed) / 1024

    return {
        "jpeg_kb": round(jpeg_kb, 1),
        "res_kb": round(res_kb, 1),
        "total_kb": round(jpeg_kb + res_kb, 1),
        "max_diff": max_diff,
        "zeros_pct": round(zeros_pct * 100, 1),
    }

def build_fingerprint(img_rgb: np.ndarray, path: str, domain: str = "unknown") -> dict:
    return {
        "path": path,
        "domain": domain,
        "timestamp": time.time(),
        "thumb16": extract_thumb(img_rgb),
        "dct8": extract_dct8(img_rgb),
        "hist": extract_hist(img_rgb),
        "stats": extract_stats(img_rgb),
        "encode": encode_stats(img_rgb),
    }

# ── Similarity ─────────────────────────────────────────

def cosine_sim(a: list, b: list) -> float:
    a, b = np.array(a), np.array(b)
    denom = (np.linalg.norm(a) * np.linalg.norm(b)) + 1e-9
    return float(np.dot(a, b) / denom)

def l2_dist(a: list, b: list) -> float:
    a, b = np.array(a), np.array(b)
    return float(np.linalg.norm(a - b))

def fingerprint_similarity(fp_a: dict, fp_b: dict) -> dict:
    """Multi-signal similarity score"""
    # thumb cosine (visual structure)
    thumb_cos = cosine_sim(fp_a["thumb16"], fp_b["thumb16"])
    # DCT cosine (frequency signature)
    dct_cos = cosine_sim(fp_a["dct8"], fp_b["dct8"])
    # hist L2 distance (color distribution)
    hist_l2 = l2_dist(fp_a["hist"], fp_b["hist"])
    hist_sim = 1.0 / (1.0 + hist_l2)

    # weighted composite
    composite = 0.4 * thumb_cos + 0.4 * dct_cos + 0.2 * hist_sim

    # S29 zone classification
    if composite > 0.95:
        zone = "FULL_REUSE"       # reuse config entirely
    elif composite > 0.80:
        zone = "STRATEGY_REUSE"   # reuse strategy, recalc q
    else:
        zone = "S27_FALLBACK"     # full encode

    return {
        "composite": round(composite, 4),
        "thumb_cos": round(thumb_cos, 4),
        "dct_cos": round(dct_cos, 4),
        "hist_sim": round(hist_sim, 4),
        "zone": zone,
    }

# ── DB operations ──────────────────────────────────────

def load_db() -> list:
    if not os.path.exists(DB_PATH):
        return []
    with open(DB_PATH, "r") as f:
        return json.load(f)

def save_db(db: list):
    with open(DB_PATH, "w") as f:
        json.dump(db, f, indent=2)

def load_image(path: str) -> np.ndarray:
    img = Image.open(path).convert("RGB")
    return np.array(img)

# ── Commands ───────────────────────────────────────────

def cmd_log(path: str, domain: str = "unknown"):
    print(f"Extracting fingerprint: {path} [domain={domain}]")
    img = load_image(path)
    fp = build_fingerprint(img, path, domain)

    db = load_db()
    # check duplicate
    for entry in db:
        if entry["path"] == path:
            print(f"  Already logged. Updating.")
            db.remove(entry)
            break

    db.append(fp)
    save_db(db)

    e = fp["encode"]
    s = fp["stats"]
    print(f"  Size:      {s['width']}×{s['height']}")
    print(f"  JPEG q95:  {e['jpeg_kb']} KB")
    print(f"  Residual:  {e['res_kb']} KB  (max_diff={e['max_diff']}, zeros={e['zeros_pct']}%)")
    print(f"  Total:     {e['total_kb']} KB")
    print(f"  Edge%:     {s['edge_pct']*100:.1f}%")
    print(f"  Variance:  {s['var']:.1f}")
    print(f"  DB size:   {len(db)} entries")

def cmd_match(path: str):
    db = load_db()
    if not db:
        print("DB empty — run 'log' first")
        return

    print(f"Matching: {path}")
    img = load_image(path)
    fp = build_fingerprint(img, path, "query")

    results = []
    for entry in db:
        if entry["path"] == path:
            continue
        sim = fingerprint_similarity(fp, entry)
        results.append((sim, entry))

    results.sort(key=lambda x: -x[0]["composite"])

    print(f"\nTop matches (DB={len(db)} entries):")
    print(f"{'Score':>8}  {'Zone':<18}  {'thumb':>7}  {'dct':>7}  Path")
    print("-" * 70)
    for sim, entry in results[:5]:
        print(f"  {sim['composite']:.4f}  {sim['zone']:<18}  "
              f"{sim['thumb_cos']:.4f}  {sim['dct_cos']:.4f}  "
              f"{os.path.basename(entry['path'])}")

    if results:
        best_sim, best = results[0]
        print(f"\n→ Best match: {best['path']}")
        print(f"  Zone: {best_sim['zone']}")
        if best_sim['zone'] == 'FULL_REUSE':
            print(f"  Action: REUSE encode config from {best['path']}")
            print(f"  Expected size: ~{best['encode']['total_kb']} KB")
        elif best_sim['zone'] == 'STRATEGY_REUSE':
            print(f"  Action: REUSE strategy, recalc q")
        else:
            print(f"  Action: S27 FALLBACK (full encode)")

def cmd_stats():
    db = load_db()
    if not db:
        print("DB empty")
        return

    print(f"Brain DB: {len(db)} entries")
    print(f"Path: {os.path.abspath(DB_PATH)}")
    print()

    # domain breakdown
    domains = {}
    for entry in db:
        d = entry.get("domain", "unknown")
        domains[d] = domains.get(d, 0) + 1
    print("Domains:")
    for d, cnt in sorted(domains.items(), key=lambda x: -x[1]):
        print(f"  {d:20s}: {cnt}")

    # encode stats
    sizes = [e["encode"]["total_kb"] for e in db]
    diffs = [e["encode"]["max_diff"] for e in db]
    zeros = [e["encode"]["zeros_pct"] for e in db]
    print(f"\nEncode stats:")
    print(f"  total_kb:  avg={np.mean(sizes):.0f}  min={np.min(sizes):.0f}  max={np.max(sizes):.0f}")
    print(f"  max_diff:  avg={np.mean(diffs):.1f}  min={np.min(diffs)}  max={np.max(diffs)}")
    print(f"  zeros%:    avg={np.mean(zeros):.1f}%")

    # similarity matrix summary
    if len(db) >= 2:
        sims = []
        for i in range(len(db)):
            for j in range(i+1, len(db)):
                s = fingerprint_similarity(db[i], db[j])
                sims.append(s["composite"])
        print(f"\nInter-image similarity:")
        print(f"  avg={np.mean(sims):.4f}  max={np.max(sims):.4f}  min={np.min(sims):.4f}")
        zones = {"FULL_REUSE": 0, "STRATEGY_REUSE": 0, "S27_FALLBACK": 0}
        for i in range(len(db)):
            for j in range(i+1, len(db)):
                s = fingerprint_similarity(db[i], db[j])
                zones[s["zone"]] += 1
        total_pairs = len(db)*(len(db)-1)//2
        print(f"  Zone distribution ({total_pairs} pairs):")
        for z, cnt in zones.items():
            print(f"    {z:<20}: {cnt}  ({cnt/total_pairs*100:.0f}%)")

def cmd_show_similar(path: str, top_n: int = 3):
    """Show most similar images with their recommended encode strategy"""
    cmd_match(path)

# ── Main ───────────────────────────────────────────────

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return

    cmd = sys.argv[1]
    if cmd == "log" and len(sys.argv) >= 3:
        domain = sys.argv[3] if len(sys.argv) > 3 else "unknown"
        cmd_log(sys.argv[2], domain)
    elif cmd == "match" and len(sys.argv) >= 3:
        cmd_match(sys.argv[2])
    elif cmd == "stats":
        cmd_stats()
    elif cmd == "show_similar" and len(sys.argv) >= 3:
        n = int(sys.argv[3]) if len(sys.argv) > 3 else 3
        cmd_show_similar(sys.argv[2], n)
    else:
        print(__doc__)

if __name__ == "__main__":
    main()
