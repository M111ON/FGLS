"""
hilbert_vault_v2.py — File → zlib+base64 SVG container (lossless)
usage:
  python hilbert_vault_v2.py encode <file>
  python hilbert_vault_v2.py decode <file.vault.svg>
  python hilbert_vault_v2.py verify <original> <decoded>
"""

import sys, os, hashlib, base64, zlib

MAGIC = 'HILBERT_VAULT_V2'

def encode(filepath):
    with open(filepath, 'rb') as f:
        data = f.read()

    sha     = hashlib.sha256(data).hexdigest()
    compressed = zlib.compress(data, level=9)
    b64     = base64.b64encode(compressed).decode()

    svg = f'''<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg"
     xmlns:vault="urn:hilbert-vault"
     width="64" height="64">
  <metadata>
    <vault:header
      magic="{MAGIC}"
      filename="{os.path.basename(filepath)}"
      original_size="{len(data)}"
      compressed_size="{len(compressed)}"
      sha256="{sha}"
    />
    <vault:data>{b64}</vault:data>
  </metadata>
  <rect width="64" height="64" fill="#0a0a0a"/>
  <text x="32" y="36" text-anchor="middle"
        font-size="8" fill="#444">VAULT</text>
</svg>'''

    out = filepath + '.vault.svg'
    with open(out, 'w') as f:
        f.write(svg)

    ratio = os.path.getsize(out) / os.path.getsize(filepath)
    print(f'encoded  → {out}')
    print(f'original={len(data)}B  compressed={len(compressed)}B  svg={os.path.getsize(out)}B  ratio={ratio:.2f}x')
    print(f'sha256={sha[:16]}...')

def decode(svgpath):
    with open(svgpath, 'r') as f:
        content = f.read()

    # extract b64
    start = content.index('<vault:data>') + len('<vault:data>')
    end   = content.index('</vault:data>')
    b64   = content[start:end].strip()

    # extract sha
    sha_start = content.index('sha256="') + len('sha256="')
    sha_end   = content.index('"', sha_start)
    sha_stored = content[sha_start:sha_end]

    # extract original size
    sz_start = content.index('original_size="') + len('original_size="')
    sz_end   = content.index('"', sz_start)
    orig_size = int(content[sz_start:sz_end])

    compressed = base64.b64decode(b64)
    data = zlib.decompress(compressed)

    sha_actual = hashlib.sha256(data).hexdigest()
    ok = sha_actual == sha_stored and len(data) == orig_size

    outpath = svgpath.replace('.vault.svg', '.decoded')
    with open(outpath, 'wb') as f:
        f.write(data)

    print(f'decoded  → {outpath}')
    print(f'size={len(data)}B  integrity={"✓ PASS" if ok else "✗ FAIL"}')

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
        print('  python hilbert_vault_v2.py encode <file>')
        print('  python hilbert_vault_v2.py decode <file.vault.svg>')
        print('  python hilbert_vault_v2.py verify <original> <decoded>')
