"""
Laplacian Pyramid - proper version
L0-L1 diff → quantize → store
L1-L2 diff → quantize → store
...
Reconstruct: walk back
"""
import numpy as np
from PIL import Image
import zlib, os, time

INPUT = "/home/geometrix0/projects/FGLS/A_highly_detailed_portrait_stu (48).png"
OUT   = "/home/geometrix0/projects/FGLS/"
os.makedirs(OUT, exist_ok=True)

img = Image.open(INPUT).convert("RGBA")
arr = np.array(img, dtype=np.float32)
H, W = arr.shape[:2]
print(f"Input: {W}x{H}  raw={arr.nbytes//1024} KB")

STEPS = 8
QUANT = 4  # quantization step — ยิ่งมาก ยิ่งบีบได้ แต่ lossy มากขึ้น

def down(a):
    h, w = a.shape[:2]
    return np.array(Image.fromarray(np.clip(a,0,255).astype(np.uint8))
                    .resize((w//2, h//2), Image.LANCZOS), dtype=np.float32)

def up(a, th, tw):
    return np.array(Image.fromarray(np.clip(a,0,255).astype(np.uint8))
                    .resize((tw, th), Image.BILINEAR), dtype=np.float32)

# ---- ENCODE ----
t0 = time.time()
levels = [arr]
for _ in range(STEPS):
    levels.append(down(levels[-1]))

# diffs[i] = levels[i] - upsample(levels[i+1])  → "what's lost going down"
diffs = []
stored_bytes = 0
for i in range(STEPS):
    h, w = levels[i].shape[:2]
    up_next = up(levels[i+1], h, w)
    diff = levels[i] - up_next  # float, centered at 0

    # Quantize: reduce precision → more zeros → zlib compresses better
    q_diff = np.round(diff / QUANT).astype(np.int8)  # int8: -128..127
    compressed = zlib.compress(q_diff.tobytes(), level=9)
    diffs.append((q_diff, compressed, levels[i].shape))
    stored_bytes += len(compressed)
    
    zeros_pct = (q_diff == 0).sum() / q_diff.size * 100
    print(f"L{i}→L{i+1}  diff {levels[i].shape[:2]}  "
          f"raw={q_diff.nbytes//1024}KB  "
          f"zlib={len(compressed)//1024}KB  "
          f"zeros={zeros_pct:.1f}%")

# base (smallest level) compressed
base_bytes = zlib.compress(levels[-1].astype(np.uint8).tobytes(), level=9)
stored_bytes += len(base_bytes)
print(f"base      {levels[-1].shape[:2]}  zlib={len(base_bytes)//1024}KB")

encode_ms = (time.time()-t0)*1000

# ---- DECODE (walk back) ----
t1 = time.time()
current = np.frombuffer(zlib.decompress(base_bytes), dtype=np.uint8)\
            .reshape(levels[-1].shape).astype(np.float32)

for i in reversed(range(5)):
    q_diff, compressed, target_shape = diffs[i]
    h, w, c = target_shape
    current = up(current, h, w)
    # dequantize
    residual = q_diff.astype(np.float32) * QUANT
    current = current + residual

recon = np.clip(current, 0, 255).astype(np.uint8)
decode_ms = (time.time()-t1)*1000

# ---- Quality ----
mse  = np.mean((arr - recon.astype(np.float32))**2)
psnr = 10*np.log10(255**2/mse) if mse > 0 else float('inf')

raw_kb    = arr.nbytes // 1024
stored_kb = stored_bytes // 1024
saving    = (1 - stored_bytes/arr.nbytes)*100

print(f"\n{'='*40}")
print(f"Original  : {raw_kb} KB")
print(f"Stored    : {stored_kb} KB  (quant={QUANT})")
print(f"Saved     : {saving:.1f}%")
print(f"PSNR      : {psnr:.1f} dB")
print(f"Encode    : {encode_ms:.0f} ms")
print(f"Decode    : {decode_ms:.0f} ms")

Image.fromarray(recon, 'RGBA').save(f"{OUT}/recon_pyramid.png")
print(f"Saved: recon_pyramid.png")
