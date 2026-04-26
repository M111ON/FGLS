"""
Maximum compression test on small image
- 8-step pyramid Q4
- L0: coordinate-only storage (sparse list, not grid)
- L1+: zlib
- Compare vs JPEG
"""
import numpy as np
from PIL import Image
import zlib, os, struct

INPUT = "/home/geometrix0/projects/FGLS/2025-12-18 19_27_image.png"
OUT   = "/home/geometrix0/projects/FGLS"
os.makedirs(OUT, exist_ok=True)

STEPS = 4
QUANT = 4

img = Image.open(INPUT).convert("RGB")
arr = np.array(img, dtype=np.float32)
H, W = arr.shape[:2]
raw_bytes = arr.nbytes
file_bytes = os.path.getsize(INPUT)
print(f"Input: {W}x{H}  file={file_bytes//1024}KB  raw={raw_bytes//1024}KB")

def down(a):
    h, w = a.shape[:2]
    if h < 2 or w < 2: return a
    return np.array(Image.fromarray(np.clip(a,0,255).astype(np.uint8))
                    .resize((max(1,w//2), max(1,h//2)), Image.LANCZOS), dtype=np.float32)

def up(a, th, tw):
    return np.array(Image.fromarray(np.clip(a,0,255).astype(np.uint8))
                    .resize((tw, th), Image.BILINEAR), dtype=np.float32)

# Build pyramid
levels = [arr]
for _ in range(STEPS):
    levels.append(down(levels[-1]))

residuals = []
for i in range(STEPS):
    h, w = levels[i].shape[:2]
    res = levels[i] - up(levels[i+1], h, w)
    q = np.clip(np.round(res / QUANT), -127, 127).astype(np.int8)
    residuals.append(q)

base = levels[-1]

# ---- Storage strategies ----
print(f"\n{'Layer':<8} {'Shape':<14} {'Zeros%':>7} {'Grid KB':>8} {'Coord KB':>9} {'zlib KB':>8} {'Best':>6}")
print("-"*65)

total_grid  = 0
total_coord = 0
total_zlib  = 0
total_best  = 0

# Base always zlib
base_c = zlib.compress(base.astype(np.uint8).tobytes(), 9)
print(f"{'base':<8} {str(base.shape[:2]):<14} {'—':>7} {'—':>8} {'—':>9} {len(base_c)//1024:>7}KB {'zlib':>6}")
total_best += len(base_c)

for i, q in enumerate(residuals):
    h, w, c = q.shape
    zeros_pct = (q == 0).sum() / q.size * 100

    # Strategy 1: full grid zlib
    grid_c = zlib.compress(q.tobytes(), 9)
    grid_kb = len(grid_c) / 1024

    # Strategy 2: coordinate list (row, col, [r,g,b]) for non-zero pixels
    nz_mask = (np.abs(q).sum(axis=2) > 0)  # (H,W) bool
    ys, xs  = np.where(nz_mask)
    vals    = q[ys, xs]  # (N, 3) int8
    # pack: uint16 y, uint16 x, int8 r, int8 g, int8 b = 7 bytes each
    coord_raw = np.zeros(len(ys), dtype=[('y','u2'),('x','u2'),('r','i1'),('g','i1'),('b','i1')])
    coord_raw['y'] = ys; coord_raw['x'] = xs
    coord_raw['r'] = vals[:,0]; coord_raw['g'] = vals[:,1]; coord_raw['b'] = vals[:,2]
    coord_c = zlib.compress(coord_raw.tobytes(), 9)
    coord_kb = len(coord_c) / 1024

    best_kb  = min(grid_kb, coord_kb)
    best_tag = "coord" if coord_kb < grid_kb else "grid"
    total_best += int(min(len(grid_c), len(coord_c)))

    total_grid  += len(grid_c)
    total_coord += len(coord_c)
    total_zlib  += len(grid_c)

    print(f"L{i:<7} {str((h,w)):<14} {zeros_pct:>6.1f}% {grid_kb:>7.1f}  {coord_kb:>8.1f}  {grid_kb:>7.1f}  {best_tag:>6}")

print("-"*65)
print(f"\n=== Compression Summary ===")
print(f"Original file (JPEG)  : {file_bytes//1024:>6} KB")
print(f"Raw uncompressed      : {raw_bytes//1024:>6} KB")
print(f"Pyramid zlib only     : {(total_zlib+len(base_c))//1024:>6} KB  ({(1-(total_zlib+len(base_c))/raw_bytes)*100:.1f}% saved vs raw)")
print(f"Pyramid best per layer: {total_best//1024:>6} KB  ({(1-total_best/raw_bytes)*100:.1f}% saved vs raw)")
print(f"Pyramid vs JPEG       : {total_best/file_bytes:.2f}x  ({'smaller' if total_best < file_bytes else 'larger'} than JPEG)")

# Reconstruct quality check
def recon_full():
    cur = base.astype(np.float32)
    for i in reversed(range(STEPS)):
        h, w = residuals[i].shape[:2]
        cur = up(cur, h, w) + residuals[i].astype(np.float32) * QUANT
    return np.clip(cur, 0, 255).astype(np.uint8)

recon = recon_full()
mse  = np.mean((arr - recon.astype(np.float32))**2)
psnr = 10*np.log10(255**2/mse) if mse > 0 else float('inf')
print(f"PSNR                  : {psnr:.1f} dB")

Image.fromarray(recon).save(f"{OUT}/fashion_recon.png")

# Compare JPEG at equivalent quality
for q_val in [85, 60, 40, 20]:
    out_path = f"{OUT}/fashion_jpeg_q{q_val}.jpg"
    img.save(out_path, "JPEG", quality=q_val)
    sz = os.path.getsize(out_path)
    print(f"JPEG q={q_val:<3}: {sz//1024:>4} KB")
