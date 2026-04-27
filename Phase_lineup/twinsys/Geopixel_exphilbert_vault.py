"""
hilbert_vault.py — File → Hilbert SVG pattern → File (lossless)
test: python hilbert_vault.py encode <file>
      python hilbert_vault.py decode <file.svg>
      python hilbert_vault.py verify <original> <decoded>
"""

import sys, os, hashlib, base64, math
import xml.etree.ElementTree as ET

# ── Config ──
CELL   = 4          # px per cell
ORDER  = 8          # Hilbert order → 256×256 = 65536 cells per tile
COLS   = ORDER * ORDER  # cells per row = 2^order

def hilbert_d2xy(n, d):
    x = y = 0
    s = 1
    while s < n:
        rx = 1 & (d >> 1)
        ry = 1 & (d ^ rx)
        if ry == 0:
            if rx == 1:
                x = s - 1 - x
                y = s - 1 - y
            x, y = y, x
        x += s * rx
        y += s * ry
        d >>= 2
        s <<= 1
    return x, y

def encode(filepath):
    with open(filepath, 'rb') as f:
        data = f.read()

    # header: original size (8 bytes) + sha256 (32 bytes) + data
    sha = hashlib.sha256(data).digest()
    size_bytes = len(data).to_bytes(8, 'big')
    payload = size_bytes + sha + data

    # pad to cell boundary
    cells_per_tile = (1 << ORDER) * (1 << ORDER)  # 65536
    total_cells = math.ceil(len(payload) / 3) * 3  # RGB triplets
    n_tiles = math.ceil(total_cells / cells_per_tile)
    padded = payload + b'\x00' * (n_tiles * cells_per_tile - len(payload))

    N = 1 << ORDER  # 256
    tile_size = N * CELL

    svgs = []
    for t in range(n_tiles):
        chunk = padded[t * cells_per_tile : (t+1) * cells_per_tile]
        svg = ET.Element('svg', {
            'xmlns': 'http://www.w3.org/2000/svg',
            'width': str(tile_size),
            'height': str(tile_size),
            'data-tile': str(t),
            'data-tiles': str(n_tiles),
            'data-order': str(ORDER),
        })
        # encode bytes as colored cells along Hilbert curve
        for d in range(cells_per_tile):
            x, y = hilbert_d2xy(N, d)
            b = chunk[d] if d < len(chunk) else 0
            # map byte to grayscale (single channel → simple)
            hex_col = f'#{b:02x}{b:02x}{b:02x}'
            ET.SubElement(svg, 'rect', {
                'x': str(x * CELL),
                'y': str(y * CELL),
                'width': str(CELL),
                'height': str(CELL),
                'fill': hex_col,
            })
        svgs.append(ET.tostring(svg, encoding='unicode'))

    out = filepath + f'.hilbert.svg'
    with open(out, 'w') as f:
        f.write('<vault tiles="' + str(n_tiles) + '">\n')
        for s in svgs:
            f.write(s + '\n')
        f.write('</vault>')

    ratio = os.path.getsize(out) / os.path.getsize(filepath)
    print(f'encoded → {out}')
    print(f'original={os.path.getsize(filepath)}B  svg={os.path.getsize(out)}B  ratio={ratio:.2f}x')
    print(f'sha256={sha.hex()[:16]}...')

def decode(svgpath):
    tree = ET.parse(svgpath)
    root = tree.getroot()
    n_tiles = int(root.attrib['tiles'])
    N = 1 << ORDER

    cells_per_tile = N * N
    all_bytes = bytearray()

    for svg in root:
        cells = [0] * cells_per_tile
        for rect in svg:
            x = int(rect.attrib['x']) // CELL
            y = int(rect.attrib['y']) // CELL
            fill = rect.attrib['fill']  # #rrggbb grayscale
            b = int(fill[1:3], 16)
            d = hilbert_xy2d(N, x, y)
            if d < cells_per_tile:
                cells[d] = b
        all_bytes.extend(cells)

    # parse header
    size = int.from_bytes(all_bytes[:8], 'big')
    sha_stored = bytes(all_bytes[8:40])
    data = bytes(all_bytes[40:40 + size])

    sha_actual = hashlib.sha256(data).digest()
    ok = sha_actual == sha_stored

    outpath = svgpath.replace('.hilbert.svg', '.decoded')
    with open(outpath, 'wb') as f:
        f.write(data)

    print(f'decoded → {outpath}')
    print(f'size={size}B  integrity={"✓ PASS" if ok else "✗ FAIL"}')

def hilbert_xy2d(n, x, y):
    d = 0
    s = n >> 1
    while s > 0:
        rx = 1 if (x & s) > 0 else 0
        ry = 1 if (y & s) > 0 else 0
        d += s * s * ((3 * rx) ^ ry)
        # rotate
        if ry == 0:
            if rx == 1:
                x = s - 1 - x
                y = s - 1 - y
            x, y = y, x
        s >>= 1
    return d

def verify(orig, decoded):
    with open(orig, 'rb') as f: a = f.read()
    with open(decoded, 'rb') as f: b = f.read()
    match = hashlib.sha256(a).hexdigest() == hashlib.sha256(b).hexdigest()
    print(f'verify: {"✓ IDENTICAL" if match else "✗ MISMATCH"}')
    print(f'  original={len(a)}B  decoded={len(b)}B')

if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else ''
    if cmd == 'encode' and len(sys.argv) > 2:
        encode(sys.argv[2])
    elif cmd == 'decode' and len(sys.argv) > 2:
        decode(sys.argv[2])
    elif cmd == 'verify' and len(sys.argv) > 3:
        verify(sys.argv[2], sys.argv[3])
    else:
        print('usage:')
        print('  python hilbert_vault.py encode <file>')
        print('  python hilbert_vault.py decode <file.hilbert.svg>')
        print('  python hilbert_vault.py verify <original> <decoded>')
