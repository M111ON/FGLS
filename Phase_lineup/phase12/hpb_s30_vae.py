"""
HPB S30 — Tiny VAE-lite
========================
Tiny autoencoder: 64×64 tile → int8 latent → reconstruct
Goal: replace flat residual with learned latent + small edge residual

Architecture:
  Encoder: RGB 64×64 → conv3 → conv3 → conv3 → flatten → FC → latent 8×8×8 (512 int8)
  Decoder: latent → FC → reshape → deconv3 → deconv3 → deconv3 → RGB 64×64

Loss: L1 + perceptual (VGG-lite via conv features)
Quantize: straight-through estimator (STE) for int8 quantization

Model size target: < 500KB
Inference: CPU realtime per tile

Usage:
  python3 hpb_s30_vae.py train <image_dir_or_ppm_list>  [--epochs N] [--domain X]
  python3 hpb_s30_vae.py encode <in.ppm|jpg> <out.lat>
  python3 hpb_s30_vae.py decode <in.lat> <out.ppm>
  python3 hpb_s30_vae.py bench  <in.ppm|jpg>
  python3 hpb_s30_vae.py demo   <in.ppm|jpg>            # show reconstructed tiles
"""

import sys, os, time, io, json, struct, glob
import numpy as np
from PIL import Image

import torch
import torch.nn as nn
import torch.nn.functional as F
from torch.utils.data import Dataset, DataLoader

# ── Config ─────────────────────────────────────────────
TILE       = 64
LATENT_CH  = 8      # channels in latent space
LATENT_SZ  = 8      # spatial size of latent (8×8)
LATENT_DIM = LATENT_CH * LATENT_SZ * LATENT_SZ  # 512
MODEL_PATH = "hpb_s30_model.pt"
DEVICE     = "cpu"

# ── Quantization (STE) ──────────────────────────────────

class STEQuantize(nn.Module):
    """Straight-through int8 quantize: forward=round, backward=identity"""
    def forward(self, x):
        # scale to [-128, 127]
        x_clamp = torch.clamp(x, -1.0, 1.0)
        if self.training:
            # STE: round in forward, pass gradient through
            x_q = torch.round(x_clamp * 127) / 127
            return x + (x_q - x).detach()
        else:
            return torch.round(x_clamp * 127) / 127

# ── Model ──────────────────────────────────────────────

class TinyEncoder(nn.Module):
    def __init__(self):
        super().__init__()
        # 64×64×3 → 32×32×16 → 16×16×32 → 8×8×64 → 8×8×8
        self.conv1 = nn.Sequential(
            nn.Conv2d(3, 16, 3, stride=2, padding=1),   # 64→32
            nn.LeakyReLU(0.1),
        )
        self.conv2 = nn.Sequential(
            nn.Conv2d(16, 32, 3, stride=2, padding=1),  # 32→16
            nn.LeakyReLU(0.1),
        )
        self.conv3 = nn.Sequential(
            nn.Conv2d(32, 64, 3, stride=2, padding=1),  # 16→8
            nn.LeakyReLU(0.1),
        )
        self.conv4 = nn.Conv2d(64, LATENT_CH, 1)        # 8×8×64 → 8×8×8
        self.quant = STEQuantize()

    def forward(self, x):
        x = self.conv1(x)
        x = self.conv2(x)
        x = self.conv3(x)
        x = self.conv4(x)
        return self.quant(torch.tanh(x))

class TinyDecoder(nn.Module):
    def __init__(self):
        super().__init__()
        # 8×8×8 → 8×8×64 → 16×16×32 → 32×32×16 → 64×64×3
        self.conv0 = nn.Conv2d(LATENT_CH, 64, 1)
        self.up1 = nn.Sequential(
            nn.Upsample(scale_factor=2, mode='nearest'),
            nn.Conv2d(64, 32, 3, padding=1),
            nn.LeakyReLU(0.1),
        )
        self.up2 = nn.Sequential(
            nn.Upsample(scale_factor=2, mode='nearest'),
            nn.Conv2d(32, 16, 3, padding=1),
            nn.LeakyReLU(0.1),
        )
        self.up3 = nn.Sequential(
            nn.Upsample(scale_factor=2, mode='nearest'),
            nn.Conv2d(16, 3, 3, padding=1),
        )

    def forward(self, z):
        x = F.leaky_relu(self.conv0(z), 0.1)
        x = self.up1(x)
        x = self.up2(x)
        x = self.up3(x)
        return torch.sigmoid(x)  # [0,1]

class TinyVAE(nn.Module):
    def __init__(self):
        super().__init__()
        self.encoder = TinyEncoder()
        self.decoder = TinyDecoder()

    def forward(self, x):
        z = self.encoder(x)
        return self.decoder(z), z

    def model_size_kb(self):
        total = sum(p.numel() * p.element_size() for p in self.parameters())
        return total / 1024

# ── Perceptual loss (lightweight) ──────────────────────

class TinyPerceptual(nn.Module):
    """2-layer fixed conv feature extractor for perceptual loss"""
    def __init__(self):
        super().__init__()
        # Sobel-like fixed edge filters
        kernel = torch.tensor([
            [[-1,-1,-1],[-1, 8,-1],[-1,-1,-1]],
            [[-1, 0, 1],[-2, 0, 2],[-1, 0, 1]],
            [[-1,-2,-1],[ 0, 0, 0],[ 1, 2, 1]],
        ], dtype=torch.float32).unsqueeze(1).expand(-1, 3, -1, -1) / 8.0
        self.register_buffer('kernel', kernel)

    def forward(self, x):
        # x: [B,3,H,W] → edge features [B,3,H,W]
        feats = F.conv2d(x, self.kernel, padding=1, groups=1)
        return feats

# ── Dataset ────────────────────────────────────────────

class TileDataset(Dataset):
    def __init__(self, images: list, augment: bool = True):
        self.tiles = []
        self.augment = augment
        for img in images:
            h, w = img.shape[:2]
            for y in range(0, h - TILE + 1, TILE // 2):  # 50% overlap stride
                for x in range(0, w - TILE + 1, TILE // 2):
                    tile = img[y:y+TILE, x:x+TILE]
                    self.tiles.append(tile.copy())
        print(f"  Dataset: {len(self.tiles)} tiles from {len(images)} images")

    def __len__(self): return len(self.tiles)

    def __getitem__(self, idx):
        tile = self.tiles[idx].astype(np.float32) / 255.0
        t = torch.from_numpy(tile).permute(2, 0, 1)  # HWC→CHW
        if self.augment and torch.rand(1) > 0.5:
            t = torch.flip(t, [2])   # horizontal flip
        if self.augment and torch.rand(1) > 0.5:
            t = torch.flip(t, [1])   # vertical flip
        return t

# ── Training ───────────────────────────────────────────

def train(image_paths: list, epochs: int = 50, domain: str = "default"):
    print(f"\n── S30 Train ─────────────────────────────────────")
    print(f"  Images: {len(image_paths)}  Epochs: {epochs}  Domain: {domain}")

    # Load images
    images = []
    for p in image_paths:
        try:
            img = np.array(Image.open(p).convert('RGB'))
            if img.shape[0] >= TILE and img.shape[1] >= TILE:
                images.append(img)
                print(f"  + {os.path.basename(p)} {img.shape[1]}×{img.shape[0]}")
        except Exception as e:
            print(f"  skip {p}: {e}")

    if not images:
        print("No valid images"); return

    dataset = TileDataset(images, augment=True)
    loader = DataLoader(dataset, batch_size=16, shuffle=True, num_workers=0)

    model = TinyVAE().to(DEVICE)
    perceptual = TinyPerceptual().to(DEVICE)
    optimizer = torch.optim.Adam(model.parameters(), lr=1e-3)
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, epochs)

    print(f"  Model size: {model.model_size_kb():.1f} KB")
    print(f"  Tiles/epoch: {len(dataset)}")
    print()

    best_loss = float('inf')
    for epoch in range(epochs):
        model.train()
        total_l1 = total_perc = total_loss = 0.0
        n_batches = 0

        for batch in loader:
            batch = batch.to(DEVICE)
            recon, z = model(batch)

            # L1 loss
            l1 = F.l1_loss(recon, batch)

            # Perceptual loss
            feat_orig = perceptual(batch)
            feat_recon = perceptual(recon)
            perc = F.l1_loss(feat_recon, feat_orig)

            loss = l1 + 0.1 * perc

            optimizer.zero_grad()
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            optimizer.step()

            total_l1 += l1.item()
            total_perc += perc.item()
            total_loss += loss.item()
            n_batches += 1

        scheduler.step()
        avg_loss = total_loss / n_batches
        avg_l1 = total_l1 / n_batches

        # PSNR estimate from L1
        psnr = -20 * np.log10(avg_l1 + 1e-8)

        if (epoch + 1) % 10 == 0 or epoch == 0:
            print(f"  Epoch {epoch+1:3d}/{epochs}  loss={avg_loss:.4f}  "
                  f"L1={avg_l1:.4f}  PSNR~{psnr:.1f}dB  "
                  f"lr={scheduler.get_last_lr()[0]:.5f}")

        if avg_loss < best_loss:
            best_loss = avg_loss
            _save_model(model, domain, avg_l1)

    print(f"\n  Best loss: {best_loss:.4f}")
    print(f"  Model saved: {MODEL_PATH}")
    _print_model_stats(model)

def _save_model(model: TinyVAE, domain: str, val_l1: float):
    torch.save({
        "model_state": model.state_dict(),
        "domain": domain,
        "val_l1": val_l1,
        "tile_size": TILE,
        "latent_dim": LATENT_DIM,
    }, MODEL_PATH)

def _print_model_stats(model: TinyVAE):
    enc_params = sum(p.numel() for p in model.encoder.parameters())
    dec_params = sum(p.numel() for p in model.decoder.parameters())
    total_params = enc_params + dec_params
    size_kb = model.model_size_kb()
    print(f"\n  Encoder params: {enc_params:,}")
    print(f"  Decoder params: {dec_params:,}")
    print(f"  Total params:   {total_params:,}")
    print(f"  Model size:     {size_kb:.1f} KB  ({'✅ <500KB' if size_kb < 500 else '⚠ >500KB'})")

# ── Inference ──────────────────────────────────────────

def load_model() -> TinyVAE:
    if not os.path.exists(MODEL_PATH):
        raise FileNotFoundError(f"No model at {MODEL_PATH} — run train first")
    ckpt = torch.load(MODEL_PATH, map_location=DEVICE, weights_only=False)
    model = TinyVAE().to(DEVICE)
    model.load_state_dict(ckpt["model_state"])
    model.eval()
    return model

def encode_tiles(model: TinyVAE, rgb: np.ndarray) -> dict:
    """Tile image → encode each tile → return latents + residual"""
    h, w = rgb.shape[:2]
    # pad to tile multiple
    ph = ((h + TILE - 1) // TILE) * TILE
    pw = ((w + TILE - 1) // TILE) * TILE
    padded = np.zeros((ph, pw, 3), dtype=np.uint8)
    padded[:h, :w] = rgb

    ny, nx = ph // TILE, pw // TILE
    latents = []
    recon_full = np.zeros((ph, pw, 3), dtype=np.float32)

    with torch.no_grad():
        for ty in range(ny):
            for tx in range(nx):
                y0, x0 = ty * TILE, tx * TILE
                tile = padded[y0:y0+TILE, x0:x0+TILE].astype(np.float32) / 255.0
                t = torch.from_numpy(tile).permute(2,0,1).unsqueeze(0)
                recon, z = model(t)
                latents.append(z.squeeze(0).numpy())
                recon_full[y0:y0+TILE, x0:x0+TILE] = recon.squeeze(0).permute(1,2,0).numpy()

    # residual = orig - recon (crop to original size)
    orig_f = padded[:h, :w].astype(np.float32) / 255.0
    recon_crop = recon_full[:h, :w]
    residual = ((orig_f - recon_crop) * 127).clip(-128, 127).astype(np.int8)

    return {
        "latents": np.array(latents, dtype=np.float32),  # [ny*nx, C, 8, 8]
        "latents_i8": (np.array(latents) * 127).clip(-128, 127).astype(np.int8),
        "residual_i8": residual,
        "recon": (recon_crop * 255).clip(0, 255).astype(np.uint8),
        "ny": ny, "nx": nx, "h": h, "w": w,
    }

def decode_tiles(model: TinyVAE, result: dict) -> np.ndarray:
    h, w = result["h"], result["w"]
    ny, nx = result["ny"], result["nx"]
    latents = result["latents_i8"].astype(np.float32) / 127.0

    recon_full = np.zeros((ny*TILE, nx*TILE, 3), dtype=np.float32)
    idx = 0
    with torch.no_grad():
        for ty in range(ny):
            for tx in range(nx):
                z = torch.from_numpy(latents[idx]).unsqueeze(0)
                recon = model.decoder(z).squeeze(0).permute(1,2,0).numpy()
                y0, x0 = ty*TILE, tx*TILE
                recon_full[y0:y0+TILE, x0:x0+TILE] = recon
                idx += 1

    recon_crop = (recon_full[:h, :w] * 255).clip(0, 255).astype(np.uint8)
    # add residual
    res = result["residual_i8"].astype(np.int16)
    final = np.clip(recon_crop.astype(np.int16) + res, 0, 255).astype(np.uint8)
    return final

# ── Commands ───────────────────────────────────────────

def cmd_bench(img_path: str):
    """Show encode quality and size breakdown"""
    model = load_model()
    rgb = np.array(Image.open(img_path).convert('RGB'))
    h, w = rgb.shape[:2]
    print(f"\nS30 bench: {img_path}  {w}×{h}")

    t0 = time.perf_counter()
    result = encode_tiles(model, rgb)
    enc_ms = (time.perf_counter() - t0) * 1000

    # measure latent + residual sizes
    import zlib
    lat_raw = result["latents_i8"].tobytes()
    lat_z = zlib.compress(lat_raw, 6)
    res_raw = result["residual_i8"].tobytes()
    res_z = zlib.compress(res_raw, 6)

    # quality
    recon = result["recon"]
    diff = np.abs(rgb.astype(np.int16) - recon.astype(np.int16))
    mse = float((diff.astype(np.float32)**2).mean())
    psnr = 10 * np.log10(255**2 / (mse + 1e-8))

    print(f"  Tiles:       {result['ny']}×{result['nx']} = {result['ny']*result['nx']}")
    print(f"  Latent raw:  {len(lat_raw)//1024} KB  → zstd: {len(lat_z)//1024} KB")
    print(f"  Residual:    {len(res_raw)//1024} KB  → zstd: {len(res_z)//1024} KB")
    print(f"  Total est:   {(len(lat_z)+len(res_z))//1024} KB")
    print(f"  PSNR:        {psnr:.1f} dB  (max_diff={diff.max()})")
    print(f"  Encode ms:   {enc_ms:.0f}ms")
    print(f"  ms/tile:     {enc_ms/(result['ny']*result['nx']):.1f}ms")

    # reconstruct with residual
    final = decode_tiles(model, result)
    diff2 = np.abs(rgb.astype(np.int16) - final.astype(np.int16))
    mse2 = float((diff2.astype(np.float32)**2).mean())
    psnr2 = 10 * np.log10(255**2 / (mse2 + 1e-8))
    print(f"\n  With residual correction:")
    print(f"  PSNR:        {psnr2:.1f} dB  (max_diff={diff2.max()})")
    print(f"  Lossless:    {'✅ YES' if diff2.max()==0 else f'❌ max_diff={diff2.max()}'}")

def cmd_demo(img_path: str):
    """Encode + decode, save side-by-side comparison"""
    model = load_model()
    rgb = np.array(Image.open(img_path).convert('RGB'))

    result = encode_tiles(model, rgb)
    final = decode_tiles(model, result)

    # save comparison
    out_path = img_path.rsplit('.', 1)[0] + '_s30_recon.png'
    h, w = rgb.shape[:2]
    comparison = np.zeros((h, w*2, 3), dtype=np.uint8)
    comparison[:, :w] = rgb
    comparison[:, w:] = final
    Image.fromarray(comparison).save(out_path)
    print(f"Saved comparison → {out_path}")
    print(f"Left: original  Right: S30 reconstruct+residual")

# ── Main ───────────────────────────────────────────────

def main():
    if len(sys.argv) < 2:
        print(__doc__); return

    cmd = sys.argv[1]

    if cmd == "train":
        # collect image paths
        paths = []
        epochs = 50
        domain = "default"
        for arg in sys.argv[2:]:
            if arg.startswith("--epochs"):
                epochs = int(arg.split("=")[1]) if "=" in arg else int(sys.argv[sys.argv.index(arg)+1])
            elif arg.startswith("--domain"):
                domain = arg.split("=")[1] if "=" in arg else sys.argv[sys.argv.index(arg)+1]
            elif os.path.isfile(arg):
                paths.append(arg)
            elif os.path.isdir(arg):
                for ext in ('*.jpg', '*.jpeg', '*.png', '*.ppm'):
                    paths.extend(glob.glob(os.path.join(arg, ext)))
        if not paths:
            print("No images found"); return
        train(paths, epochs=epochs, domain=domain)

    elif cmd == "bench" and len(sys.argv) >= 3:
        cmd_bench(sys.argv[2])

    elif cmd == "demo" and len(sys.argv) >= 3:
        cmd_demo(sys.argv[2])

    else:
        print(__doc__)

if __name__ == "__main__":
    main()
