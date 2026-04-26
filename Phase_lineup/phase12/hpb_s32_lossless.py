"""
HPB S32 — Lossless Delta Reconstruct
=====================================
Concept: JPEG เป็น low-res base (เร็ว เล็ก) + sparse delta เพื่อ reconstruct raw exact

Format (.hpb32):
  [4B magic "HPB2"]
  [4B header_len] [JSON header]
  per tile (stream-friendly, sequential):
    [4B tile_delta_len] [delta bytes zstd]
  [4B jpeg_len] [JPEG bytes]   ← ท้ายสุด เพื่อให้ tile stream ได้ก่อน

Tile delta:
  - เก็บเฉพาะ pixel ที่ diff != 0 (sparse)
  - format: [N: uint16] [N × (y:uint16, x:uint16, r:int8, g:int8, b:int8)]
  - ถ้า all-zero tile → tile_delta_len = 0 (skip entirely)

Stream: decode tile-by-tile ได้ทันทีโดยไม่ต้องรอ full file

Usage:
  python3 hpb_s32_lossless.py encode <in.jpg|png|ppm> <out.hpb32> [--jpeg-q 85] [--threshold 0]
  python3 hpb_s32_lossless.py decode <in.hpb32> <out.png>
  python3 hpb_s32_lossless.py bench  <in.jpg|png|ppm>
  python3 hpb_s32_lossless.py stream <in.hpb32>   # demo tile-by-tile stream
"""

import sys, os, io, json, struct, time, zlib
import numpy as np
from PIL import Image

# ── optional zstd ────────────────────────────────────────
try:
    import zstandard as zstd
    def _compress(data): return zstd.ZstdCompressor(level=3).compress(data)
    def _decompress(data): return zstd.ZstdDecompressor().decompress(data)
    COMPRESSOR = "zstd"
except ImportError:
    def _compress(data): return zlib.compress(data, 6)
    def _decompress(data): return zlib.decompress(data)
    COMPRESSOR = "zlib"

MAGIC     = b"HPB2"
TILE      = 64   # tile size — ปรับได้, 64 balance ระหว่าง stream granularity กับ overhead

# ── sparse delta encode/decode ───────────────────────────

def encode_tile_delta(delta_tile: np.ndarray) -> bytes:
    """
    delta_tile: (H, W, 3) int16  range [-255, 255]
    คืน bytes ของ sparse list เฉพาะ pixel ที่ != 0
    ถ้า all-zero คืน b""
    """
    mask = np.any(delta_tile != 0, axis=2)  # (H, W) bool
    ys, xs = np.where(mask)
    N = len(ys)
    if N == 0:
        return b""

    buf = bytearray()
    buf += struct.pack('<H', N)  # uint16 count
    for y, x in zip(ys, xs):
        r, g, b = delta_tile[y, x]
        buf += struct.pack('<HHbbb', y, x, int(r), int(g), int(b))
    return bytes(buf)

def decode_tile_delta(data: bytes, tile_h: int, tile_w: int) -> np.ndarray:
    """คืน (H, W, 3) int16"""
    out = np.zeros((tile_h, tile_w, 3), dtype=np.int16)
    if not data:
        return out
    N = struct.unpack_from('<H', data, 0)[0]
    offset = 2
    for _ in range(N):
        y, x, r, g, b = struct.unpack_from('<HHbbb', data, offset)
        out[y, x] = (r, g, b)
        offset += 7
    return out

# ── encode ───────────────────────────────────────────────

def encode(src_path: str, dst_path: str, jpeg_q: int = 85, threshold: int = 0):
    """
    threshold=0  → lossless exact
    threshold>0  → lossy sparse (ลด delta size แลก reconstruct ไม่ exact)
    """
    rgb = np.array(Image.open(src_path).convert('RGB'))
    h, w = rgb.shape[:2]

    # JPEG base
    buf = io.BytesIO()
    Image.fromarray(rgb).save(buf, format='JPEG', quality=jpeg_q, optimize=True, subsampling=0)
    jpeg_bytes = buf.getvalue()
    jpeg_rgb = np.array(Image.open(io.BytesIO(jpeg_bytes)).convert('RGB'))

    # pad to tile boundary
    ph = ((h + TILE - 1) // TILE) * TILE
    pw = ((w + TILE - 1) // TILE) * TILE
    padded_orig = np.zeros((ph, pw, 3), dtype=np.uint8)
    padded_orig[:h, :w] = rgb
    padded_jpeg = np.zeros((ph, pw, 3), dtype=np.uint8)
    padded_jpeg[:h, :w] = jpeg_rgb

    ny, nx = ph // TILE, pw // TILE

    # per-tile delta
    tile_blobs = []  # list of compressed bytes (b"" = all-zero tile)
    total_nonzero_pixels = 0
    total_pixels = h * w

    for ty in range(ny):
        for tx in range(nx):
            y0, x0 = ty * TILE, tx * TILE
            orig_t = padded_orig[y0:y0+TILE, x0:x0+TILE].astype(np.int16)
            jpeg_t = padded_jpeg[y0:y0+TILE, x0:x0+TILE].astype(np.int16)
            delta  = orig_t - jpeg_t  # range [-255, 255]

            if threshold > 0:
                delta[np.abs(delta) < threshold] = 0  # lossy option

            raw = encode_tile_delta(delta)
            if raw:
                total_nonzero_pixels += struct.unpack_from('<H', raw, 0)[0]
                compressed = _compress(raw)
            else:
                compressed = b""
            tile_blobs.append(compressed)

    # header
    header = {
        "version":    32,
        "w": w, "h": h,
        "jpeg_q":     jpeg_q,
        "threshold":  threshold,
        "tile":       TILE,
        "ny": ny, "nx": nx,
        "compressor": COMPRESSOR,
        "lossless":   threshold == 0,
    }
    hdr_bytes = json.dumps(header).encode()

    # write — tile deltas ก่อน, JPEG ท้ายสุด (stream decode ไม่ต้องการ JPEG ทั้งก้อนก่อน)
    with open(dst_path, 'wb') as f:
        f.write(MAGIC)
        f.write(struct.pack('>I', len(hdr_bytes))); f.write(hdr_bytes)
        for blob in tile_blobs:
            f.write(struct.pack('>I', len(blob))); f.write(blob)
        f.write(struct.pack('>I', len(jpeg_bytes))); f.write(jpeg_bytes)

    total_sz  = os.path.getsize(dst_path)
    delta_sz  = sum(len(b) for b in tile_blobs)
    zero_tiles = sum(1 for b in tile_blobs if not b)
    sparse_pct = 100 * (1 - total_nonzero_pixels / total_pixels)

    print(f"  Image:    {w}×{h}  tiles={ny*nx} ({ny}×{nx})")
    print(f"  JPEG base:{len(jpeg_bytes)//1024}KB  q={jpeg_q}  compressor={COMPRESSOR}")
    print(f"  Delta:    {delta_sz//1024}KB  zero_tiles={zero_tiles}/{ny*nx}  sparse={sparse_pct:.1f}%")
    print(f"  Total:    {total_sz//1024}KB  → {dst_path}")
    print(f"  Lossless: {threshold == 0}")

# ── decode (full) ─────────────────────────────────────────

def decode(src_path: str, dst_path: str) -> np.ndarray:
    with open(src_path, 'rb') as f:
        assert f.read(4) == MAGIC, "Not HPB32"
        hdr_len = struct.unpack('>I', f.read(4))[0]
        header  = json.loads(f.read(hdr_len))
        w, h    = header['w'], header['h']
        ny, nx  = header['ny'], header['nx']
        tile_sz = header['tile']
        ph, pw  = ny * tile_sz, nx * tile_sz

        # read all tile blobs
        tile_blobs = []
        for _ in range(ny * nx):
            sz = struct.unpack('>I', f.read(4))[0]
            tile_blobs.append(f.read(sz))

        # read JPEG
        jpeg_len  = struct.unpack('>I', f.read(4))[0]
        jpeg_bytes = f.read(jpeg_len)

    jpeg_rgb = np.array(Image.open(io.BytesIO(jpeg_bytes)).convert('RGB'))
    padded = np.zeros((ph, pw, 3), dtype=np.uint8)
    padded[:h, :w] = jpeg_rgb

    idx = 0
    for ty in range(ny):
        for tx in range(nx):
            y0, x0 = ty * tile_sz, tx * tile_sz
            ey = min(y0 + tile_sz, h)
            ex = min(x0 + tile_sz, w)
            th = ey - y0
            tw = ex - x0

            blob = tile_blobs[idx]
            if blob:
                raw   = _decompress(blob)
                delta = decode_tile_delta(raw, tile_sz, tile_sz)
                tile  = padded[y0:y0+tile_sz, x0:x0+tile_sz].astype(np.int16)
                tile += delta
                padded[y0:y0+tile_sz, x0:x0+tile_sz] = np.clip(tile, 0, 255).astype(np.uint8)
            idx += 1

    final = padded[:h, :w]
    Image.fromarray(final).save(dst_path)
    print(f"Decoded → {dst_path}")
    return final

# ── stream decode (tile-by-tile, no full buffer needed) ──

def stream_decode(src_path: str):
    """
    Generator: yield (ty, tx, tile_rgb_uint8) ทีละ tile
    ไม่ต้องโหลด full image ก่อน — ใช้ได้กับ network stream
    Note: JPEG base อยู่ท้ายไฟล์ ดังนั้นต้อง seek ไปอ่านก่อน
          ในระบบ real streaming ส่ง JPEG base เป็น first packet แทน
    """
    with open(src_path, 'rb') as f:
        assert f.read(4) == MAGIC
        hdr_len = struct.unpack('>I', f.read(4))[0]
        header  = json.loads(f.read(hdr_len))
        w, h    = header['w'], header['h']
        ny, nx  = header['ny'], header['nx']
        tile_sz = header['tile']

        # collect tile blob positions
        tile_offsets = []
        for _ in range(ny * nx):
            pos = f.tell()
            sz  = struct.unpack('>I', f.read(4))[0]
            tile_offsets.append((pos, sz))
            f.seek(sz, 1)

        # read JPEG (ท้ายสุด)
        jpeg_len   = struct.unpack('>I', f.read(4))[0]
        jpeg_bytes = f.read(jpeg_len)

        jpeg_rgb = np.array(Image.open(io.BytesIO(jpeg_bytes)).convert('RGB'))
        ph, pw   = ny * tile_sz, nx * tile_sz
        padded   = np.zeros((ph, pw, 3), dtype=np.uint8)
        padded[:h, :w] = jpeg_rgb

        # stream tiles
        for idx, (ty, tx) in enumerate([(r, c) for r in range(ny) for c in range(nx)]):
            pos, sz = tile_offsets[idx]
            f.seek(pos + 4)
            blob = f.read(sz)

            y0, x0 = ty * tile_sz, tx * tile_sz
            if blob:
                raw   = _decompress(blob)
                delta = decode_tile_delta(raw, tile_sz, tile_sz)
                tile  = padded[y0:y0+tile_sz, x0:x0+tile_sz].astype(np.int16)
                tile += delta
                padded[y0:y0+tile_sz, x0:x0+tile_sz] = np.clip(tile, 0, 255).astype(np.uint8)

            yield ty, tx, padded[y0:y0+tile_sz, x0:x0+tile_sz].copy()

# ── bench ─────────────────────────────────────────────────

def bench(src_path: str):
    rgb = np.array(Image.open(src_path).convert('RGB'))
    h, w = rgb.shape[:2]
    print(f"\nS32 bench: {src_path}  {w}×{h}\n")

    tmp_enc = "/tmp/_s32_bench.hpb32"
    tmp_dec = "/tmp/_s32_bench_dec.png"

    for q in [75, 85, 92]:
        t0 = time.perf_counter()
        encode(src_path, tmp_enc, jpeg_q=q, threshold=0)
        enc_ms = (time.perf_counter() - t0) * 1000

        t1 = time.perf_counter()
        final = decode(tmp_enc, tmp_dec)
        dec_ms = (time.perf_counter() - t1) * 1000

        exact   = np.array_equal(rgb, final)
        sz      = os.path.getsize(tmp_enc) // 1024
        print(f"  q={q} → {sz}KB  exact={exact}  enc={enc_ms:.0f}ms  dec={dec_ms:.0f}ms\n")

    # reference
    for q in [75, 85]:
        buf = io.BytesIO()
        Image.fromarray(rgb).save(buf, format='JPEG', quality=q)
        print(f"  REF JPEG q={q}: {len(buf.getvalue())//1024}KB  (lossy)")

    buf_png = io.BytesIO()
    Image.fromarray(rgb).save(buf_png, format='PNG')
    print(f"  REF PNG (lossless): {len(buf_png.getvalue())//1024}KB")

# ── main ──────────────────────────────────────────────────

def main():
    if len(sys.argv) < 2:
        print(__doc__); return
    cmd = sys.argv[1]

    if cmd == "encode" and len(sys.argv) >= 4:
        q     = int(next((a.split('=')[1] for a in sys.argv if '--jpeg-q' in a), 85))
        thr   = int(next((a.split('=')[1] for a in sys.argv if '--threshold' in a), 0))
        encode(sys.argv[2], sys.argv[3], jpeg_q=q, threshold=thr)

    elif cmd == "decode" and len(sys.argv) >= 4:
        decode(sys.argv[2], sys.argv[3])

    elif cmd == "bench" and len(sys.argv) >= 3:
        bench(sys.argv[2])

    elif cmd == "stream" and len(sys.argv) >= 3:
        print(f"Streaming {sys.argv[2]} tile-by-tile:")
        for ty, tx, tile in stream_decode(sys.argv[2]):
            print(f"  tile ({ty},{tx}) shape={tile.shape} mean={tile.mean():.1f}")

    else:
        print(__doc__)

if __name__ == "__main__":
    main()
