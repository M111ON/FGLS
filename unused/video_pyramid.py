"""
Video Spatial+Temporal Pyramid Compression
- Spatial: L0→L1→L2 pyramid per frame
- Temporal: frame[t] - frame[t-1] diff
- Store: base + spatial residuals + temporal residuals
- Reconstruct: walk back both dimensions
"""
import numpy as np
import cv2
import zlib, os, time

INPUT = "/mnt/user-data/uploads/7a9fde57271e4f5bb2e193f63460de77.mp4"
OUT   = "/mnt/user-data/outputs"
os.makedirs(OUT, exist_ok=True)

SPATIAL_STEPS = 3
QUANT_SPATIAL = 4
QUANT_TEMPORAL = 2   # temporal diff ละเอียดกว่า spatial
MAX_FRAMES = 60      # test แค่ 60 frames ก่อน (~2s)

# ---- helpers ----
def down(a):
    h, w = a.shape[:2]
    return cv2.resize(a, (w//2, h//2), interpolation=cv2.INTER_LANCZOS4).astype(np.float32)

def up(a, th, tw):
    return cv2.resize(a, (tw, th), interpolation=cv2.INTER_LINEAR).astype(np.float32)

def spatial_encode(frame_f):
    """frame → base + spatial residuals"""
    levels = [frame_f]
    for _ in range(SPATIAL_STEPS):
        levels.append(down(levels[-1]))
    base = levels[-1]
    residuals = []
    for i in range(SPATIAL_STEPS):
        h, w = levels[i].shape[:2]
        residuals.append(levels[i] - up(levels[i+1], h, w))
    return base, residuals

def spatial_decode(base, residuals):
    cur = base.copy()
    for res in reversed(residuals):
        h, w = res.shape[:2]
        cur = up(cur, h, w) + res
    return np.clip(cur, 0, 255)

def compress_arr(arr_int8):
    return zlib.compress(arr_int8.tobytes(), level=6)

def decompress_arr(data, shape, dtype):
    return np.frombuffer(zlib.decompress(data), dtype=dtype).reshape(shape)

# ---- Read frames ----
cap = cv2.VideoCapture(INPUT)
fps = cap.get(cv2.CAP_PROP_FPS)
W   = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
H   = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))

frames = []
while len(frames) < MAX_FRAMES:
    ret, f = cap.read()
    if not ret: break
    frames.append(cv2.cvtColor(f, cv2.COLOR_BGR2RGB).astype(np.float32))
cap.release()

N = len(frames)
raw_bytes = sum(f.nbytes for f in frames)
print(f"Frames: {N}  {W}x{H}  raw={raw_bytes//1024}KB  fps={fps:.0f}")

# ---- ENCODE ----
t0 = time.time()

stored_chunks = []   # list of compressed bytes
stored_bytes  = 0

prev_base = None
prev_residuals = None

spatial_res_bytes  = 0
temporal_res_bytes = 0
base_bytes_total   = 0

for fi, frame in enumerate(frames):
    base, spatials = spatial_encode(frame)
    
    # --- temporal diff on base ---
    if prev_base is None:
        # keyframe: store base raw
        q_base = base.astype(np.uint8)
        cb = compress_arr(q_base)
        stored_chunks.append(('keyframe_base', cb, base.shape))
        base_bytes_total += len(cb)
    else:
        # temporal diff on base
        tdiff = base - prev_base
        q_tdiff = np.clip(np.round(tdiff / QUANT_TEMPORAL), -127, 127).astype(np.int8)
        cb = compress_arr(q_tdiff)
        stored_chunks.append(('temporal_base', cb, base.shape))
        temporal_res_bytes += len(cb)
    
    # --- spatial residuals: temporal diff too ---
    for li, res in enumerate(spatials):
        if prev_residuals is None:
            q_res = np.clip(np.round(res / QUANT_SPATIAL), -127, 127).astype(np.int8)
            cb = compress_arr(q_res)
            stored_chunks.append((f'keyframe_res{li}', cb, res.shape))
            spatial_res_bytes += len(cb)
        else:
            # diff residual between frames
            tres = res - prev_residuals[li]
            q_tres = np.clip(np.round(tres / QUANT_SPATIAL), -127, 127).astype(np.int8)
            cb = compress_arr(q_tres)
            stored_chunks.append((f'temporal_res{li}', cb, res.shape))
            temporal_res_bytes += len(cb)
    
    prev_base      = base.copy()
    prev_residuals = [s.copy() for s in spatials]
    
    stored_bytes = base_bytes_total + spatial_res_bytes + temporal_res_bytes

encode_ms = (time.time()-t0)*1000

# ---- DECODE ----
t1 = time.time()

recon_frames = []
cur_base = None
cur_residuals = [None]*SPATIAL_STEPS

chunk_idx = 0
for fi in range(N):
    chunks_this_frame = 1 + SPATIAL_STEPS  # base + residuals
    
    # base
    tag, cb, shape = stored_chunks[chunk_idx]; chunk_idx+=1
    if tag == 'keyframe_base':
        cur_base = decompress_arr(cb, shape, np.uint8).astype(np.float32)
    else:
        tdiff = decompress_arr(cb, shape, np.int8).astype(np.float32) * QUANT_TEMPORAL
        cur_base = cur_base + tdiff
    
    # residuals
    for li in range(SPATIAL_STEPS):
        tag, cb, shape = stored_chunks[chunk_idx]; chunk_idx+=1
        if tag.startswith('keyframe'):
            cur_residuals[li] = decompress_arr(cb, shape, np.int8).astype(np.float32) * QUANT_SPATIAL
        else:
            tres = decompress_arr(cb, shape, np.int8).astype(np.float32) * QUANT_SPATIAL
            cur_residuals[li] = cur_residuals[li] + tres
    
    recon = spatial_decode(cur_base, cur_residuals)
    recon_frames.append(np.clip(recon, 0, 255).astype(np.uint8))

decode_ms = (time.time()-t1)*1000

# ---- Quality ----
mses = [np.mean((frames[i] - recon_frames[i].astype(np.float32))**2) for i in range(N)]
avg_mse  = np.mean(mses)
avg_psnr = 10*np.log10(255**2/avg_mse) if avg_mse > 0 else float('inf')

saving = (1 - stored_bytes/raw_bytes)*100

print(f"\n{'='*45}")
print(f"Raw         : {raw_bytes//1024:>8} KB")
print(f"Base store  : {base_bytes_total//1024:>8} KB")
print(f"Spatial res : {spatial_res_bytes//1024:>8} KB")
print(f"Temporal res: {temporal_res_bytes//1024:>8} KB")
print(f"Total stored: {stored_bytes//1024:>8} KB")
print(f"Saved       : {saving:.1f}%")
print(f"PSNR avg    : {avg_psnr:.1f} dB")
print(f"Encode      : {encode_ms:.0f} ms  ({encode_ms/N:.1f}ms/frame)")
print(f"Decode      : {decode_ms:.0f} ms  ({decode_ms/N:.1f}ms/frame)")

# ---- Save sample frames ----
for i in [0, N//2, N-1]:
    cv2.imwrite(f"{OUT}/frame_{i:03d}_orig.png",
                cv2.cvtColor(frames[i].astype(np.uint8), cv2.COLOR_RGB2BGR))
    cv2.imwrite(f"{OUT}/frame_{i:03d}_recon.png",
                cv2.cvtColor(recon_frames[i], cv2.COLOR_RGB2BGR))

# temporal zeros analysis
print(f"\n--- Temporal Sparsity (sample) ---")
for fi in range(1, min(5, N)):
    tdiff = frames[fi] - frames[fi-1]
    zeros = (np.abs(tdiff) < QUANT_TEMPORAL).sum() / tdiff.size * 100
    print(f"  frame {fi-1}→{fi}: {zeros:.1f}% near-zero")
