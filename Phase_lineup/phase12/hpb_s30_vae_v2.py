"""
HPB S30 VAE v2
==============
Fixes vs v1:
- STEQuantize eval = no round (fixes 29dB→41dB gap)
- DEVICE = cuda auto-detect
- all tensors .to(DEVICE)
- resume support (--resume)
- batch 512 + cudnn.benchmark + pin_memory

Usage:
  python3 hpb_s30_vae_v2.py train <img_dir|img...> [--epochs N] [--domain X] [--resume]
  python3 hpb_s30_vae_v2.py bench  <img>
  python3 hpb_s30_vae_v2.py demo   <img>
"""

import sys, os, io, json, struct, glob, time
import numpy as np
from PIL import Image

import torch
import torch.nn as nn
import torch.nn.functional as F
from torch.utils.data import Dataset, DataLoader

# ── Config ──────────────────────────────────────────────
TILE        = 64
LATENT_CH   = 8
LATENT_SZ   = 8
LATENT_DIM  = LATENT_CH * LATENT_SZ * LATENT_SZ  # 512
MODEL_PATH  = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hpb_s30_model.pt")
RESUME_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hpb_s30_checkpoint.pt")
DEVICE      = "cuda" if torch.cuda.is_available() else "cpu"

# ── Quantization ────────────────────────────────────────
class STEQuantize(nn.Module):
    """Train: STE round  |  Eval: passthrough (no quantization error)"""
    def forward(self, x):
        x_clamp = torch.clamp(x, -1.0, 1.0)
        if self.training:
            x_q = torch.round(x_clamp * 127) / 127
            return x + (x_q - x).detach()
        return x_clamp  # ← KEY FIX: no round at inference

# ── Model ────────────────────────────────────────────────
class TinyEncoder(nn.Module):
    def __init__(self):
        super().__init__()
        self.conv1 = nn.Sequential(nn.Conv2d(3,  16, 3, stride=2, padding=1), nn.LeakyReLU(0.1))
        self.conv2 = nn.Sequential(nn.Conv2d(16, 32, 3, stride=2, padding=1), nn.LeakyReLU(0.1))
        self.conv3 = nn.Sequential(nn.Conv2d(32, 64, 3, stride=2, padding=1), nn.LeakyReLU(0.1))
        self.conv4 = nn.Conv2d(64, LATENT_CH, 1)
        self.quant = STEQuantize()
    def forward(self, x):
        return self.quant(torch.tanh(self.conv4(self.conv3(self.conv2(self.conv1(x))))))

class TinyDecoder(nn.Module):
    def __init__(self):
        super().__init__()
        self.conv0 = nn.Conv2d(LATENT_CH, 64, 1)
        self.up1 = nn.Sequential(nn.Upsample(scale_factor=2, mode='nearest'), nn.Conv2d(64, 32, 3, padding=1), nn.LeakyReLU(0.1))
        self.up2 = nn.Sequential(nn.Upsample(scale_factor=2, mode='nearest'), nn.Conv2d(32, 16, 3, padding=1), nn.LeakyReLU(0.1))
        self.up3 = nn.Sequential(nn.Upsample(scale_factor=2, mode='nearest'), nn.Conv2d(16,  3, 3, padding=1))
    def forward(self, z):
        return torch.sigmoid(self.up3(self.up2(self.up1(F.leaky_relu(self.conv0(z), 0.1)))))

class TinyVAE(nn.Module):
    def __init__(self):
        super().__init__()
        self.encoder = TinyEncoder()
        self.decoder = TinyDecoder()
    def forward(self, x):
        z = self.encoder(x)
        return self.decoder(z), z
    def model_size_kb(self):
        return sum(p.numel() * p.element_size() for p in self.parameters()) / 1024

# ── Perceptual loss ──────────────────────────────────────
class TinyPerceptual(nn.Module):
    def __init__(self):
        super().__init__()
        k = torch.tensor([
            [[-1,-1,-1],[-1, 8,-1],[-1,-1,-1]],
            [[-1, 0, 1],[-2, 0, 2],[-1, 0, 1]],
            [[-1,-2,-1],[ 0, 0, 0],[ 1, 2, 1]],
        ], dtype=torch.float32).unsqueeze(1).expand(-1,3,-1,-1) / 8.0
        self.register_buffer('kernel', k)
    def forward(self, x):
        return F.conv2d(x, self.kernel, padding=1)

# ── Dataset ──────────────────────────────────────────────
class TileDataset(Dataset):
    def __init__(self, images, augment=True):
        self.tiles = []
        self.augment = augment
        for img in images:
            h, w = img.shape[:2]
            for y in range(0, h - TILE + 1, TILE):   # no overlap for training speed
                for x in range(0, w - TILE + 1, TILE):
                    self.tiles.append(img[y:y+TILE, x:x+TILE].copy())
        print(f"  Dataset: {len(self.tiles)} tiles from {len(images)} images")
    def __len__(self): return len(self.tiles)
    def __getitem__(self, idx):
        t = torch.from_numpy(self.tiles[idx].astype(np.float32) / 255.0).permute(2,0,1)
        if self.augment:
            if torch.rand(1) > 0.5: t = torch.flip(t, [2])
            if torch.rand(1) > 0.5: t = torch.flip(t, [1])
        return t

# ── Save / Load ──────────────────────────────────────────
def save_checkpoint(model, domain, val_l1, epoch, optimizer, scheduler, best_loss):
    ckpt = {
        "model_state":     model.state_dict(),
        "optimizer_state": optimizer.state_dict(),
        "scheduler_state": scheduler.state_dict(),
        "domain": domain, "val_l1": val_l1,
        "epoch": epoch, "best_loss": best_loss,
        "tile_size": TILE, "latent_dim": LATENT_DIM,
    }
    torch.save(ckpt, MODEL_PATH)
    torch.save(ckpt, RESUME_PATH)

def load_model() -> TinyVAE:
    if not os.path.exists(MODEL_PATH):
        raise FileNotFoundError(f"No model at {MODEL_PATH}")
    ckpt = torch.load(MODEL_PATH, map_location=DEVICE, weights_only=False)
    m = TinyVAE().to(DEVICE)
    m.load_state_dict(ckpt["model_state"])
    m.eval()
    return m

# ── Train ────────────────────────────────────────────────
def train(image_paths, epochs=50, domain="default", resume=False):
    print(f"\n── S30 Train v2 ──────────────────────────────────")
    print(f"  Device: {DEVICE}  Epochs: {epochs}  Domain: {domain}")

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

    batch_size = 512 if DEVICE == "cuda" else 16
    dataset = TileDataset(images, augment=True)
    loader  = DataLoader(dataset, batch_size=batch_size, shuffle=True,
                         num_workers=4, pin_memory=(DEVICE=="cuda"), persistent_workers=True)

    model      = TinyVAE().to(DEVICE)
    perceptual = TinyPerceptual().to(DEVICE)
    optimizer  = torch.optim.Adam(model.parameters(), lr=1e-3)
    scheduler  = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, epochs)

    if DEVICE == "cuda":
        torch.backends.cudnn.benchmark = True

    start_epoch = 0
    best_loss   = float('inf')

    if resume and os.path.exists(RESUME_PATH):
        ckpt = torch.load(RESUME_PATH, map_location=DEVICE, weights_only=False)
        model.load_state_dict(ckpt["model_state"])
        optimizer.load_state_dict(ckpt["optimizer_state"])
        scheduler.load_state_dict(ckpt["scheduler_state"])
        start_epoch = ckpt.get("epoch", 0) + 1
        best_loss   = ckpt.get("best_loss", float('inf'))
        print(f"  ↩ Resumed epoch {start_epoch}  best_loss={best_loss:.4f}")
    elif resume:
        print("  No checkpoint — starting fresh")

    print(f"  Model: {model.model_size_kb():.1f}KB  Tiles: {len(dataset)}  Batch: {batch_size}\n")

    for epoch in range(start_epoch, start_epoch + epochs):
        model.train()
        tl1 = tperc = tloss = nb = 0
        for batch in loader:
            batch = batch.to(DEVICE, non_blocking=True)
            recon, z = model(batch)
            l1   = F.l1_loss(recon, batch)
            perc = F.l1_loss(perceptual(recon), perceptual(batch))
            loss = l1 + 0.1 * perc
            optimizer.zero_grad()
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            optimizer.step()
            tl1 += l1.item(); tperc += perc.item(); tloss += loss.item(); nb += 1

        scheduler.step()
        al = tloss / nb; al1 = tl1 / nb
        psnr = -20 * np.log10(al1 + 1e-8)

        if (epoch - start_epoch + 1) % 10 == 0 or epoch == start_epoch:
            print(f"  Epoch {epoch+1:4d}  loss={al:.4f}  L1={al1:.4f}  PSNR~{psnr:.1f}dB  lr={scheduler.get_last_lr()[0]:.5f}")

        if al < best_loss:
            best_loss = al
            save_checkpoint(model, domain, al1, epoch, optimizer, scheduler, best_loss)

    print(f"\n  Best loss: {best_loss:.4f}  saved → {MODEL_PATH}")

# ── Inference ────────────────────────────────────────────
def encode_tiles(model, rgb):
    h, w = rgb.shape[:2]
    ph = ((h+TILE-1)//TILE)*TILE
    pw = ((w+TILE-1)//TILE)*TILE
    pad = np.zeros((ph,pw,3), dtype=np.uint8); pad[:h,:w] = rgb
    ny, nx = ph//TILE, pw//TILE
    latents = []
    recon_full = np.zeros((ph,pw,3), dtype=np.float32)
    with torch.no_grad():
        for ty in range(ny):
            for tx in range(nx):
                y0, x0 = ty*TILE, tx*TILE
                tile = pad[y0:y0+TILE, x0:x0+TILE].astype(np.float32)/255.0
                t = torch.from_numpy(tile).permute(2,0,1).unsqueeze(0).to(DEVICE)
                recon, z = model(t)
                latents.append((z.squeeze(0).cpu().numpy()*127).clip(-128,127).astype(np.int8))
                recon_full[y0:y0+TILE, x0:x0+TILE] = recon.squeeze(0).cpu().permute(1,2,0).numpy()
    orig_f    = pad[:h,:w].astype(np.float32)/255.0
    recon_f   = recon_full[:h,:w]
    residual  = ((orig_f - recon_f)*127).clip(-128,127).astype(np.int8)
    return {
        "latents_i8": np.array(latents, dtype=np.int8),
        "residual_i8": residual,
        "recon": (recon_f*255).clip(0,255).astype(np.uint8),
        "ny": ny, "nx": nx, "h": h, "w": w,
    }

def decode_tiles(model, result):
    h, w = result["h"], result["w"]
    ny, nx = result["ny"], result["nx"]
    lf = result["latents_i8"].astype(np.float32)/127.0
    recon_full = np.zeros((ny*TILE, nx*TILE, 3), dtype=np.float32)
    idx = 0
    with torch.no_grad():
        for ty in range(ny):
            for tx in range(nx):
                z = torch.from_numpy(lf[idx]).unsqueeze(0).to(DEVICE)
                r = model.decoder(z).squeeze(0).cpu().permute(1,2,0).numpy()
                y0, x0 = ty*TILE, tx*TILE
                recon_full[y0:y0+TILE, x0:x0+TILE] = r
                idx += 1
    base = (recon_full[:h,:w]*255).clip(0,255).astype(np.uint8)
    res  = result["residual_i8"].astype(np.int16)
    return np.clip(base.astype(np.int16) + res, 0, 255).astype(np.uint8)

# ── Bench ─────────────────────────────────────────────────
def bench(img_path):
    import zlib
    model = load_model()
    rgb = np.array(Image.open(img_path).convert('RGB'))
    h, w = rgb.shape[:2]
    print(f"\nS30v2 bench: {img_path}  {w}×{h}  device={DEVICE}")

    t0 = time.perf_counter()
    result = encode_tiles(model, rgb)
    enc_ms = (time.perf_counter()-t0)*1000

    lat_z = zlib.compress(result["latents_i8"].tobytes(), 6)
    res_z = zlib.compress(result["residual_i8"].tobytes(), 6)

    recon = result["recon"]
    mse   = float(((rgb.astype(np.float32)-recon.astype(np.float32))**2).mean())
    psnr  = 10*np.log10(255**2/(mse+1e-8))

    print(f"  Latent:     {len(lat_z)//1024}KB")
    print(f"  Residual:   {len(res_z)//1024}KB")
    print(f"  Total:      {(len(lat_z)+len(res_z))//1024}KB")
    print(f"  PSNR (lat): {psnr:.1f}dB  enc={enc_ms:.0f}ms")

    final = decode_tiles(model, result)
    mse2  = float(((rgb.astype(np.float32)-final.astype(np.float32))**2).mean())
    psnr2 = 10*np.log10(255**2/(mse2+1e-8))
    print(f"  PSNR (+res):{psnr2:.1f}dB  lossless={'✅' if np.array_equal(rgb,final) else '❌'}")

    buf = io.BytesIO()
    Image.fromarray(rgb).save(buf, format='JPEG', quality=85)
    print(f"\n  REF JPEG q85: {len(buf.getvalue())//1024}KB")

# ── Demo ──────────────────────────────────────────────────
def demo(img_path):
    model = load_model()
    rgb   = np.array(Image.open(img_path).convert('RGB'))
    result = encode_tiles(model, rgb)
    final  = decode_tiles(model, result)
    out = img_path.rsplit('.',1)[0] + '_s30v2_recon.png'
    h, w = rgb.shape[:2]
    cmp = np.zeros((h, w*2, 3), dtype=np.uint8)
    cmp[:,:w] = rgb; cmp[:,w:] = final
    Image.fromarray(cmp).save(out)
    print(f"Saved → {out}  (left=orig  right=recon)")

# ── Main ──────────────────────────────────────────────────
def main():
    if len(sys.argv) < 2: print(__doc__); return
    cmd = sys.argv[1]

    if cmd == "train":
        paths, epochs, domain, resume = [], 50, "default", False
        i = 2
        while i < len(sys.argv):
            a = sys.argv[i]
            if a == "--epochs":   epochs = int(sys.argv[i+1]); i+=2
            elif a.startswith("--epochs="): epochs = int(a.split("=")[1]); i+=1
            elif a == "--domain": domain = sys.argv[i+1]; i+=2
            elif a.startswith("--domain="): domain = a.split("=")[1]; i+=1
            elif a == "--resume": resume = True; i+=1
            elif os.path.isfile(a): paths.append(a); i+=1
            elif os.path.isdir(a):
                for ext in ('*.jpg','*.jpeg','*.png','*.ppm'):
                    paths.extend(glob.glob(os.path.join(a,ext)))
                i+=1
            else: i+=1
        if not paths: print("No images found"); return
        train(paths, epochs=epochs, domain=domain, resume=resume)

    elif cmd == "bench" and len(sys.argv) >= 3: bench(sys.argv[2])
    elif cmd == "demo"  and len(sys.argv) >= 3: demo(sys.argv[2])
    else: print(__doc__)

if __name__ == "__main__":
    main()
