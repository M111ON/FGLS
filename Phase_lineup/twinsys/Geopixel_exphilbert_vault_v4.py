"""
hilbert_vault_v4.py — Folder → single .vault.svgz (lossless)
─────────────────────────────────────────────────────────────
Changes from v3:
  1. Visual layer: data bits → QR-style <rect> grid embedded in SVG
     (structured pattern = gzip bites hard on repetitive XML)
  2. Outer gzip: entire SVG → .svgz  (~30-60% smaller than v3 plain SVG)
  3. Removed dead decode_unused() function
  4. decode auto-detects .svgz vs .svg (backward compat with v3)
  5. encode writes .vault.svgz instead of .vault.svg

Pipeline:
  ENCODE: file_data → zlib(9) → base64 → metadata XML
          all_data_bits → binary grid → SVG <rect> cells
          full SVG → gzip → .vault.svgz

  DECODE: .svgz → gunzip → parse metadata → base64 → zlib decompress

Usage:
  python hilbert_vault_v4.py encode <folder>
  python hilbert_vault_v4.py decode <file.vault.svgz> [output_dir]
  python hilbert_vault_v4.py decode <file.vault.svg>  [output_dir]  ← v3 compat
  python hilbert_vault_v4.py verify <original_folder> <decoded_folder>
"""

import sys, os, gzip, math, hashlib, base64, zlib, re
from pathlib import Path

MAGIC      = 'HILBERT_VAULT_V4'
GRID_COLOR = '#1a1a2e'  # background


# ── QR visual layer (PNG 1-bit embedded as data URI) ─────────────
# SVG <rect> per bit = XML bloat on binary data (50% bits=1 → too many rects)
# PNG 1-bit row-per-row + zlib inside PNG = far smaller for binary payload

def _build_qr_png_b64(payload_bytes: bytes, cols: int = 256) -> tuple:
    """
    payload_bytes → 1-bit PNG (cols wide) as base64 string.
    PNG internal zlib handles structured patterns efficiently.
    Returns (png_b64, cols, rows)
    """
    import struct, zlib as _zlib

    bits = []
    for byte in payload_bytes:
        for i in range(7, -1, -1):
            bits.append((byte >> i) & 1)

    # pad to full rows
    rows = math.ceil(len(bits) / cols)
    bits += [0] * (rows * cols - len(bits))

    def png_chunk(tag, data):
        crc = _zlib.crc32(tag + data) & 0xFFFFFFFF
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', crc)

    # IHDR: 1-bit grayscale
    ihdr = struct.pack('>IIBBBBB', cols, rows, 1, 0, 0, 0, 0)
    chunks = [b'\x89PNG\r\n\x1a\n', png_chunk(b'IHDR', ihdr)]

    # IDAT: scanlines with filter byte 0
    raw_rows = []
    for r in range(rows):
        row_bits = bits[r*cols:(r+1)*cols]
        # pack 8 bits per byte
        row_bytes = bytearray()
        for c in range(0, cols, 8):
            byte = 0
            for b in range(8):
                if c + b < cols:
                    byte = (byte << 1) | row_bits[c + b]
                else:
                    byte <<= 1
            row_bytes.append(byte)
        raw_rows.append(b'\x00' + bytes(row_bytes))

    idat_raw = b''.join(raw_rows)
    idat_comp = _zlib.compress(idat_raw, 9)
    chunks.append(png_chunk(b'IDAT', idat_comp))
    chunks.append(png_chunk(b'IEND', b''))

    png_bytes = b''.join(chunks)
    return base64.b64encode(png_bytes).decode(), cols, rows


# ── encode ────────────────────────────────────────────────────────

def encode(folderpath):
    folder = Path(folderpath)
    files  = sorted([f for f in folder.rglob('*') if f.is_file()])
    if not files:
        print('no files found'); return

    total_orig = 0
    total_comp = 0
    file_nodes = []

    # compress each file
    for fpath in files:
        data       = fpath.read_bytes()
        sha        = hashlib.sha256(data).hexdigest()
        compressed = zlib.compress(data, level=9)
        b64        = base64.b64encode(compressed).decode()
        rel        = str(fpath.relative_to(folder))
        total_orig += len(data)
        total_comp += len(compressed)
        file_nodes.append((rel, len(data), len(compressed), sha, b64))

    folder_sha = hashlib.sha256(
        ''.join(sha for _, _, _, sha, _ in file_nodes).encode()
    ).hexdigest()

    # ── QR visual: encode folder_sha as visual fingerprint (not data duplicate)
    # Data already lives in <vault:file> metadata — QR is identity stamp only
    # folder_sha (32 bytes) → 256 bits → 16×16 grid = clean, tiny, meaningful
    sha_bytes = bytes.fromhex(folder_sha)   # 32 bytes = 256 bits
    PNG_COLS  = 16
    CELL_PX   = 16   # 16px per bit → 256×256px final image (QR-sized)
    png_b64, grid_cols, grid_rows = _build_qr_png_b64(sha_bytes, PNG_COLS)

    SVG_W    = PNG_COLS * CELL_PX
    GRID_H   = grid_rows * CELL_PX
    LABEL_H  = 32
    SVG_H    = GRID_H + LABEL_H

    # ── assemble SVG XML
    lines = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        f'<svg xmlns="http://www.w3.org/2000/svg" xmlns:vault="urn:hilbert-vault"'
        f' width="{SVG_W}" height="{SVG_H}">',
        '  <metadata>',
        f'    <vault:folder',
        f'      magic="{MAGIC}"',
        f'      name="{folder.name}"',
        f'      files="{len(file_nodes)}"',
        f'      original_size="{total_orig}"',
        f'      compressed_size="{total_comp}"',
        f'      sha256="{folder_sha}"',
        f'      grid_cols="{grid_cols}"',
        f'      grid_rows="{grid_rows}"',
        f'    />',
    ]
    for rel, orig, comp, sha, b64 in file_nodes:
        lines.append(
            f'    <vault:file name="{rel}" original_size="{orig}"'
            f' compressed_size="{comp}" sha256="{sha}">'
        )
        lines.append(f'      {b64}')
        lines.append('    </vault:file>')
    lines += [
        '  </metadata>',
        f'  <rect width="{SVG_W}" height="{SVG_H}" fill="{GRID_COLOR}"/>',
        # 1-bit PNG embedded as image — clean, no per-rect XML bloat
        f'  <image x="0" y="0" width="{SVG_W}" height="{GRID_H}"'
        f' image-rendering="pixelated"'
        f' href="data:image/png;base64,{png_b64}"/>',
        f'  <text x="{SVG_W//2}" y="{GRID_H + 14}" text-anchor="middle"'
        f' font-size="10" fill="#555">{folder.name}</text>',
        f'  <text x="{SVG_W//2}" y="{GRID_H + 26}" text-anchor="middle"'
        f' font-size="8" fill="#333">{len(file_nodes)} files · {total_orig}B → {total_comp}B</text>',
        '</svg>',
    ]

    svg_text = '\n'.join(lines)

    # ── gzip outer layer
    out = str(folder) + '.vault.svgz'
    with gzip.open(out, 'wb', compresslevel=9) as f:
        f.write(svg_text.encode('utf-8'))

    svgz_size = os.path.getsize(out)
    ratio     = svgz_size / total_orig if total_orig else 0
    print(f'encoded  → {out}')
    print(f'files={len(file_nodes)}  original={total_orig}B  zlib={total_comp}B'
          f'  svgz={svgz_size}B  ratio={ratio:.3f}x')
    print(f'grid={grid_cols}×{grid_rows} cells ({SVG_W}×{SVG_H}px)')
    print(f'folder_sha={folder_sha[:16]}...')


# ── decode ────────────────────────────────────────────────────────

def _read_svg(svgpath: str) -> str:
    """Read .svgz (gzipped) or plain .svg (v3 compat)."""
    with open(svgpath, 'rb') as f:
        raw = f.read()
    # gzip magic bytes: 1f 8b
    if raw[:2] == b'\x1f\x8b':
        return gzip.decompress(raw).decode('utf-8')
    return raw.decode('utf-8')

def decode(svgpath, outdir=None):
    content = _read_svg(svgpath)

    def get_attr(tag, attr):
        idx = content.index(f'{attr}="', content.index(tag)) + len(f'{attr}="')
        return content[idx:content.index('"', idx)]

    folder_name = get_attr('vault:folder', 'name')
    n_files     = int(get_attr('vault:folder', 'files'))

    base = Path(outdir) if outdir else \
           Path(svgpath).parent / (folder_name + '_decoded')
    base.mkdir(parents=True, exist_ok=True)

    file_blocks = re.findall(
        r'<vault:file(.*?)>(.*?)</vault:file>', content, re.DOTALL
    )

    ok_count = 0
    for attrs_str, b64_raw in file_blocks:
        def ga(a):
            m = re.search(f'{a}="([^"]*)"', attrs_str)
            return m.group(1) if m else ''
        rel        = ga('name')
        sha_stored = ga('sha256')
        orig_size  = int(ga('original_size'))
        b64        = b64_raw.strip()

        compressed = base64.b64decode(b64)
        data       = zlib.decompress(compressed)
        sha_actual = hashlib.sha256(data).hexdigest()
        ok         = sha_actual == sha_stored and len(data) == orig_size

        outpath = base / rel
        outpath.parent.mkdir(parents=True, exist_ok=True)
        outpath.write_bytes(data)

        status = '✓' if ok else '✗'
        print(f'  {status} {rel}  ({len(data)}B)')
        if ok: ok_count += 1

    print(f'\ndecoded → {base}')
    print(f'files={ok_count}/{n_files}'
          f'  {"✓ ALL PASS" if ok_count == n_files else "✗ SOME FAILED"}')


# ── verify ────────────────────────────────────────────────────────

def verify(orig_folder, decoded_folder):
    orig    = Path(orig_folder)
    decoded = Path(decoded_folder)
    files   = sorted([f for f in orig.rglob('*') if f.is_file()])

    ok = 0
    for f in files:
        rel = f.relative_to(orig)
        d   = decoded / rel
        if not d.exists():
            print(f'  ✗ MISSING {rel}'); continue
        match = f.read_bytes() == d.read_bytes()
        print(f'  {"✓" if match else "✗"} {rel}')
        if match: ok += 1

    print(f'\nverify: {ok}/{len(files)}'
          f'  {"✓ ALL PASS" if ok == len(files) else "✗ MISMATCH"}')


# ── main ──────────────────────────────────────────────────────────

if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else ''
    if cmd == 'encode' and len(sys.argv) > 2:
        encode(sys.argv[2])
    elif cmd == 'decode' and len(sys.argv) > 2:
        decode(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None)
    elif cmd == 'verify' and len(sys.argv) > 3:
        verify(sys.argv[2], sys.argv[3])
    else:
        print('usage:')
        print('  python hilbert_vault_v4.py encode <folder>')
        print('  python hilbert_vault_v4.py decode <file.vault.svgz>')
        print('  python hilbert_vault_v4.py verify <original_folder> <decoded_folder>')
