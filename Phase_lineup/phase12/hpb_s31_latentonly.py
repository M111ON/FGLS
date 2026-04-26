"""
HPB S31 — Hybrid Final Form
============================
JPEG q75 base + S30 latent correction + selective edge residual

Format (.hpb31):
  [4B magic] [4B header_len] [JSON header]
  [4B jpeg_len] [JPEG bytes]
  [4B lat_len]  [latent int8 zstd]
  [4B res_len]  [residual int8 zstd | 0 if skipped]

Usage:
  python3 hpb_s31_hybrid.py encode <in.jpg|ppm> <out.hpb31> [--psnr-floor 38] [--jpeg-q 75]
  python3 hpb_s31_hybrid.py decode <in.hpb31> <out.png>
  python3 hpb_s31_hybrid.py bench  <in.jpg|ppm>
"""

import sys, os, io, json, struct, time, zlib
import numpy as np
from PIL import Image

# ── optional zstd (fallback zlib) ───────────────────────
try:
    import zstandard as zstd
    def _compress(data): return zstd.ZstdCompressor(level=3).compress(data)
    def _decompress(data): return zstd.ZstdDecompressor().decompress(data)
    COMPRESSOR = "zstd"
except ImportError:
    def _compress(data): return zlib.compress(data, 6)
    def _decompress(data): return zlib.decompress(data)
    COMPRESSOR = "zlib"

import torch
import torch.nn as nn
import torch.nn.functional as F

# ── S30 model (inline, no import dependency) ────────────
TILE      = 64
LATENT_CH = 8
LATENT_SZ = 8
S30_PATH  = "hpb_s30_model.pt"
DEVICE = "cpu"
MAGIC     = b"HPB1"

class STEQuantize(nn.Module):
    def forward(self, x):
        x_clamp = torch.clamp(x, -1.0, 1.0)
        if self.training:
            x_q = torch.round(x_clamp * 127) / 127
            return x + (x_q - x).detach()
        return torch.round(x_clamp * 127) / 127

class TinyEncoder(nn.Module):
    def __init__(self):
        super().__init__()
        self.conv1 = nn.Sequential(nn.Conv2d(3,16,3,stride=2,padding=1), nn.LeakyReLU(0.1))
        self.conv2 = nn.Sequential(nn.Conv2d(16,32,3,stride=2,padding=1), nn.LeakyReLU(0.1))
        self.conv3 = nn.Sequential(nn.Conv2d(32,64,3,stride=2,padding=1), nn.LeakyReLU(0.1))
        self.conv4 = nn.Conv2d(64, LATENT_CH, 1)
        self.quant = STEQuantize()
    def forward(self, x):
        return self.quant(torch.tanh(self.conv4(self.conv3(self.conv2(self.conv1(x))))))

class TinyDecoder(nn.Module):
    def __init__(self):
        super().__init__()
        self.conv0 = nn.Conv2d(LATENT_CH, 64, 1)
        self.up1 = nn.Sequential(nn.Upsample(scale_factor=2,mode='nearest'), nn.Conv2d(64,32,3,padding=1), nn.LeakyReLU(0.1))
        self.up2 = nn.Sequential(nn.Upsample(scale_factor=2,mode='nearest'), nn.Conv2d(32,16,3,padding=1), nn.LeakyReLU(0.1))
        self.up3 = nn.Sequential(nn.Upsample(scale_factor=2,mode='nearest'), nn.Conv2d(16,3,3,padding=1))
    def forward(self, z):
        return torch.sigmoid(self.up3(self.up2(self.up1(F.leaky_relu(self.conv0(z),0.1)))))

class TinyVAE(nn.Module):
    def __init__(self):
        super().__init__()
        self.encoder = TinyEncoder()
        self.decoder = TinyDecoder()
    def forward(self, x):
        z = self.encoder(x)
        return self.decoder(z), z

# ── helpers ─────────────────────────────────────────────

def load_s30():
    if not os.path.exists(S30_PATH):
        return None
    ckpt = torch.load(S30_PATH, map_location=DEVICE, weights_only=False)
    m = TinyVAE().to(DEVICE)
    m.load_state_dict(ckpt["model_state"])
    m.eval()
    return m

def psnr_np(a, b):
    mse = float(((a.astype(np.float32) - b.astype(np.float32))**2).mean())
    return 10 * np.log10(255**2 / (mse + 1e-8))

def tile_psnr(orig_tile, recon_tile):
    return psnr_np(orig_tile, recon_tile)

# ── encode ───────────────────────────────────────────────

def encode(src_path: str, dst_path: str, jpeg_q: int = 75, psnr_floor: float = 38.0):
    rgb = np.array(Image.open(src_path).convert('RGB'))
    h, w = rgb.shape[:2]

    # ── Layer 1: JPEG base ──
    buf = io.BytesIO()
    Image.fromarray(rgb).save(buf, format='JPEG', quality=jpeg_q, optimize=True)
    jpeg_bytes = buf.getvalue()
    jpeg_rgb = np.array(Image.open(io.BytesIO(jpeg_bytes)).convert('RGB'))

    # ── Layer 2: S30 latent correction ──
    model = load_s30()
    has_latent = model is not None

    ph = ((h + TILE - 1) // TILE) * TILE
    pw = ((w + TILE - 1) // TILE) * TILE
    ny, nx = ph // TILE, pw // TILE

    padded_orig = np.zeros((ph, pw, 3), dtype=np.uint8)
    padded_orig[:h, :w] = rgb
    padded_jpeg = np.zeros((ph, pw, 3), dtype=np.uint8)
    padded_jpeg[:h, :w] = jpeg_rgb

    # Residual delta: orig - jpeg_base (stored as-is, used even without S30)
    delta = (padded_orig.astype(np.int16) - padded_jpeg.astype(np.int16))[:h, :w]

    latents_i8 = None
    tile_skip_mask = None  # per-tile residual skip flags

    if has_latent:
        latents = []
        recon_lat = np.zeros((ph, pw, 3), dtype=np.float32)

        with torch.no_grad():
            for ty in range(ny):
                for tx in range(nx):
                    y0, x0 = ty*TILE, tx*TILE
                    # encode from orig tile
                    tile = padded_orig[y0:y0+TILE, x0:x0+TILE].astype(np.float32)/255.0
                    t = torch.from_numpy(tile).permute(2,0,1).unsqueeze(0)
                    recon, z = model(t)
                    latents.append((z.squeeze(0).numpy()*127).clip(-128,127).astype(np.int8))
                    recon_lat[y0:y0+TILE, x0:x0+TILE] = recon.squeeze(0).permute(1,2,0).numpy()

        latents_i8 = np.array(latents, dtype=np.int8)  # [ny*nx, C, 8, 8]

        # per-tile PSNR after latent: decide skip residual
        tile_skip_mask = []
        recon_lat_u8 = (recon_lat*255).clip(0,255).astype(np.uint8)
        skipped = 0
        for ty in range(ny):
            for tx in range(nx):
                y0, x0 = ty*TILE, tx*TILE
                ey, ex = min(y0+TILE,h), min(x0+TILE,w)
                if ey <= 0 or ex <= 0:
                    tile_skip_mask.append(1); skipped += 1; continue
                p = tile_psnr(padded_orig[y0:ey, x0:ex], recon_lat_u8[y0:ey, x0:ex])
                skip = 1 if p >= psnr_floor else 0
                tile_skip_mask.append(skip)
                skipped += skip

        # selective residual: zero out skipped tiles
        res_i8 = delta.clip(-128,127).astype(np.int8).copy()
        # recompute delta from latent recon (better base for residual)
        lat_crop = recon_lat_u8[:h, :w]
        delta_lat = (rgb.astype(np.int16) - lat_crop.astype(np.int16)).clip(-128,127).astype(np.int8)

        for idx, (ty, tx) in enumerate([(r,c) for r in range(ny) for c in range(nx)]):
            y0, x0 = ty*TILE, tx*TILE
            ey, ex = min(y0+TILE,h), min(x0+TILE,w)
            if tile_skip_mask[idx]:
                delta_lat[y0:ey, x0:ex] = 0
        res_i8 = delta_lat

        print(f"  Tiles: {ny*nx}  skipped residual: {skipped}/{ny*nx} ({100*skipped//(ny*nx)}%)")
    else:
        # no S30 — store full JPEG residual
        res_i8 = delta.clip(-128,127).astype(np.int8)
        print(f"  No S30 model — storing full JPEG residual")

    # ── compress layers ──
    lat_z  = _compress(latents_i8.tobytes()) if has_latent else b""
    res_z  = _compress(res_i8.tobytes())

    # ── pack .hpb31 ──
    header = {
        "version": 31,
        "w": w, "h": h,
        "jpeg_q": jpeg_q,
        "psnr_floor": psnr_floor,
        "has_latent": has_latent,
        "ny": ny, "nx": nx,
        "tile_skip": tile_skip_mask,
        "compressor": COMPRESSOR,
    }
    hdr_bytes = json.dumps(header).encode()

    with open(dst_path, 'wb') as f:
        f.write(MAGIC)
        f.write(struct.pack('>I', len(hdr_bytes))); f.write(hdr_bytes)
        f.write(struct.pack('>I', len(jpeg_bytes))); f.write(jpeg_bytes)
        f.write(struct.pack('>I', len(lat_z)));     f.write(lat_z)
        f.write(struct.pack('>I', len(res_z)));     f.write(res_z)

    total = os.path.getsize(dst_path)
    psnr_base = psnr_np(rgb, jpeg_rgb)
    print(f"  JPEG base:  {len(jpeg_bytes)//1024} KB  PSNR~{psnr_base:.1f}dB")
    print(f"  Latent:     {len(lat_z)//1024} KB")
    print(f"  Residual:   {len(res_z)//1024} KB")
    print(f"  Total:      {total//1024} KB  → {dst_path}")

# ── decode ───────────────────────────────────────────────

def decode(src_path: str, dst_path: str):
    with open(src_path, 'rb') as f:
        assert f.read(4) == MAGIC, "Not HPB31"
        hdr_len = struct.unpack('>I', f.read(4))[0]
        header  = json.loads(f.read(hdr_len))
        jpeg_len= struct.unpack('>I', f.read(4))[0]; jpeg_bytes = f.read(jpeg_len)
        lat_len = struct.unpack('>I', f.read(4))[0]; lat_z  = f.read(lat_len)
        res_len = struct.unpack('>I', f.read(4))[0]; res_z  = f.read(res_len)

    w, h   = header['w'], header['h']
    ny, nx = header['ny'], header['nx']
    has_latent = header['has_latent']

    jpeg_rgb = np.array(Image.open(io.BytesIO(jpeg_bytes)).convert('RGB'))

    if has_latent and lat_z:
        model = load_s30()
        lat_raw = _decompress(lat_z)
        latents_i8 = np.frombuffer(lat_raw, dtype=np.int8).reshape(ny*nx, LATENT_CH, LATENT_SZ, LATENT_SZ).copy()
        latents_f = latents_i8.astype(np.float32) / 127.0

        recon_lat = np.zeros((ny*TILE, nx*TILE, 3), dtype=np.float32)
        with torch.no_grad():
            for idx, (ty, tx) in enumerate([(r,c) for r in range(ny) for c in range(nx)]):
                z = torch.from_numpy(latents_f[idx]).unsqueeze(0)
                recon = model.decoder(z).squeeze(0).permute(1,2,0).numpy()
                y0, x0 = ty*TILE, tx*TILE
                recon_lat[y0:y0+TILE, x0:x0+TILE] = recon

        base_u8 = (recon_lat[:h, :w]*255).clip(0,255).astype(np.uint8)
    else:
        base_u8 = jpeg_rgb

    # add residual
    res_raw = _decompress(res_z)
    res_i8  = np.frombuffer(res_raw, dtype=np.int8).reshape(h, w, 3).copy()
    final   = np.clip(base_u8.astype(np.int16) + res_i8.astype(np.int16), 0, 255).astype(np.uint8)

    Image.fromarray(final).save(dst_path)
    print(f"Decoded → {dst_path}")

# ── bench ────────────────────────────────────────────────

def bench(src_path: str):
    rgb = np.array(Image.open(src_path).convert('RGB'))
    h, w = rgb.shape[:2]
    print(f"\nS31 bench: {src_path}  {w}×{h}")

    tmp = "/tmp/_s31_bench.hpb31"
    for q in [65, 75, 85]:
        for floor in [36, 38, 40]:
            t0 = time.perf_counter()
            # redirect prints
            import contextlib, io as sio
            buf = sio.StringIO()
            with contextlib.redirect_stdout(buf):
                encode(src_path, tmp, jpeg_q=q, psnr_floor=floor)
            enc_ms = (time.perf_counter()-t0)*1000

            decode(tmp, "/tmp/_s31_bench_dec.png")
            dec_rgb = np.array(Image.open("/tmp/_s31_bench_dec.png").convert('RGB'))
            p = psnr_np(rgb, dec_rgb)
            sz = os.path.getsize(tmp)//1024
            skip_line = [l for l in buf.getvalue().splitlines() if 'skipped' in l]
            skip_info = skip_line[0].strip() if skip_line else ""
            print(f"  q={q} floor={floor}dB → {sz}KB  PSNR={p:.1f}dB  {enc_ms:.0f}ms  {skip_info}")

    # ── latent-only test ──
    print(f"\n── Latent-only mode ──")
    model = load_s30()
    if model:
        ph = ((h+TILE-1)//TILE)*TILE
        pw = ((w+TILE-1)//TILE)*TILE
        ny2, nx2 = ph//TILE, pw//TILE
        padded = np.zeros((ph,pw,3),dtype=np.uint8); padded[:h,:w]=rgb
        latents=[]; recon_lat=np.zeros((ph,pw,3),dtype=np.float32)
        with torch.no_grad():
            for ty in range(ny2):
                for tx in range(nx2):
                    y0,x0=ty*TILE,tx*TILE
                    tile=padded[y0:y0+TILE,x0:x0+TILE].astype(np.float32)/255.0
                    t=torch.from_numpy(tile).permute(2,0,1).unsqueeze(0)
                    recon,z=model(t)
                    latents.append((z.squeeze(0).numpy()*127).clip(-128,127).astype(np.int8))
                    recon_lat[y0:y0+TILE,x0:x0+TILE]=recon.squeeze(0).permute(1,2,0).numpy()
        lat_i8=np.array(latents,dtype=np.int8)
        lat_z=_compress(lat_i8.tobytes())
        recon_u8=(recon_lat[:h,:w]*255).clip(0,255).astype(np.uint8)
        p=psnr_np(rgb,recon_u8)
        print(f"  Latent size: {len(lat_z)//1024}KB  PSNR={p:.1f}dB  tiles={ny2*nx2}")
        if p>=40:   print(f"  → ✅ latent-only path viable")
        elif p>=35: print(f"  → edge residual only needed")
        else:       print(f"  → ❌ need full residual")

    # reference
    buf2 = io.BytesIO()
    Image.fromarray(rgb).save(buf2, format='JPEG', quality=85)
    print(f"\n  REF JPEG q85: {len(buf2.getvalue())//1024}KB  PSNR~38dB")

# ── main ─────────────────────────────────────────────────

def main():
    if len(sys.argv) < 2:
        print(__doc__); return
    cmd = sys.argv[1]

    if cmd == "encode" and len(sys.argv) >= 4:
        q     = int(next((a.split('=')[1] for a in sys.argv if '--jpeg-q' in a), 75))
        floor = float(next((a.split('=')[1] for a in sys.argv if '--psnr-floor' in a), 38.0))
        encode(sys.argv[2], sys.argv[3], jpeg_q=q, psnr_floor=floor)

    elif cmd == "decode" and len(sys.argv) >= 4:
        decode(sys.argv[2], sys.argv[3])

    elif cmd == "bench" and len(sys.argv) >= 3:
        bench(sys.argv[2])

    else:
        print(__doc__)

if __name__ == "__main__":
    main()
