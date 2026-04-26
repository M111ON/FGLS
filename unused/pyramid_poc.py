"""
Proof of Concept: Multi-scale Residual Compression
640 → 320 → 160 → 80  (Laplacian Pyramid)
"""
import numpy as np
from PIL import Image
import os, sys

# --- สร้างภาพ test ถ้าไม่มีของจริง ---
def make_test_image(size=640):
    img = np.zeros((size, size, 3), dtype=np.uint8)
    # gradient bg
    for i in range(size):
        img[i, :, 0] = int(i * 255 / size)
        img[:, i, 1] = int(i * 200 / size)
    # "subject" block (simulate foreground)
    cx, cy = size//2, size//2
    img[cy-80:cy+80, cx-80:cx+80, 2] = 200
    img[cy-20:cy+20, cx-20:cx+20] = [255, 255, 255]
    return img

# --- Core: Pyramid Build ---
def build_pyramid(img_np, steps=3):
    """ลดขนาดทีละ 1/2, เก็บ residual แต่ละชั้น"""
    levels = [img_np.astype(np.float32)]
    for _ in range(steps):
        prev = levels[-1]
        h, w = prev.shape[:2]
        small = np.array(Image.fromarray(prev.astype(np.uint8)).resize((w//2, h//2), Image.LANCZOS), dtype=np.float32)
        levels.append(small)
    
    # Laplacian = residual ที่แต่ละชั้น
    residuals = []
    for i in range(len(levels)-1):
        h, w = levels[i].shape[:2]
        upsampled = np.array(Image.fromarray(levels[i+1].astype(np.uint8)).resize((w, h), Image.BILINEAR), dtype=np.float32)
        residual = levels[i] - upsampled  # [-255, 255]
        residuals.append(residual)
    
    base = levels[-1]  # 80x80 — โครงสร้างหลัก
    return base, residuals

# --- Reconstruct ---
def reconstruct(base, residuals):
    current = base.astype(np.float32)
    for res in reversed(residuals):
        h, w = res.shape[:2]
        current = np.array(Image.fromarray(np.clip(current, 0, 255).astype(np.uint8)).resize((w, h), Image.BILINEAR), dtype=np.float32)
        current = current + res
    return np.clip(current, 0, 255).astype(np.uint8)

# --- Entropy (bits per pixel estimate) ---
def entropy(arr):
    flat = arr.flatten()
    vals, counts = np.unique(flat, return_counts=True)
    probs = counts / len(flat)
    probs = probs[probs > 0]
    return -np.sum(probs * np.log2(probs))

# --- Main ---
img_np = make_test_image(640)
original_size = img_np.nbytes

base, residuals = build_pyramid(img_np, steps=3)
# sizes: 640, 320, 160, 80

print("=== Multi-scale Residual Analysis ===")
print(f"Original  640x640 : {original_size/1024:.1f} KB  entropy={entropy(img_np):.2f} bits/px")
print(f"Base       80x80  : {base.nbytes/1024:.1f} KB")

total_residual_bytes = sum(r.nbytes for r in residuals)

for i, res in enumerate(residuals):
    scale = 640 // (2**i)
    # activity = how much residual (high = important zone)
    activity = np.abs(res).mean()
    e = entropy(np.clip(res + 128, 0, 255).astype(np.uint8))
    print(f"Residual L{i+1} {scale}x{scale}: {res.nbytes/1024:.1f} KB  activity={activity:.2f}  entropy={e:.2f}")

# --- Selective drop: ทิ้ง residual ที่ activity ต่ำ ---
THRESHOLD = 2.0  # ถ้า activity < นี้ → ทิ้ง
kept = [r if np.abs(r).mean() >= THRESHOLD else np.zeros_like(r) for r in residuals]
kept_bytes = sum(r.nbytes if np.abs(residuals[i]).mean() >= THRESHOLD else 0 
               for i, r in enumerate(kept))

print(f"\n--- Compression Estimate ---")
print(f"Stored = base + kept residuals")
stored = base.nbytes + kept_bytes
ratio = (1 - stored / original_size) * 100
print(f"Stored size : {stored/1024:.1f} KB  (vs {original_size/1024:.1f} KB original)")
print(f"Space saved : {ratio:.1f}%")

# Verify reconstruct quality
recon = reconstruct(base, kept)
mse = np.mean((img_np.astype(float) - recon.astype(float))**2)
psnr = 10 * np.log10(255**2 / mse) if mse > 0 else float('inf')
print(f"PSNR        : {psnr:.1f} dB  ({'lossless-ish' if psnr > 40 else 'lossy but ok' if psnr > 30 else 'degraded'})")

# Save visual
out_dir = "/mnt/user-data/outputs"
os.makedirs(out_dir, exist_ok=True)
Image.fromarray(img_np).save(f"{out_dir}/original.png")
Image.fromarray(base.astype(np.uint8)).save(f"{out_dir}/base_80.png")
Image.fromarray(recon).save(f"{out_dir}/reconstructed.png")
# Residual heatmap (L1 = finest)
res_vis = np.clip(residuals[0] + 128, 0, 255).astype(np.uint8)
Image.fromarray(res_vis).save(f"{out_dir}/residual_L1.png")
print("\nImages saved to outputs/")
