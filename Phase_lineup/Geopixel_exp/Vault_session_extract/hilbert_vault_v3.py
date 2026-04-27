"""
hilbert_vault_v3.py — Folder → single SVG vault (lossless)
usage:
  python hilbert_vault_v3.py encode <folder>
  python hilbert_vault_v3.py decode <folder.vault.svg> [output_dir]
  python hilbert_vault_v3.py verify <original_folder> <decoded_folder>
"""

import sys, os, hashlib, base64, zlib
import xml.etree.ElementTree as ET
from pathlib import Path

MAGIC = 'HILBERT_VAULT_V3'

def encode(folderpath):
    folder = Path(folderpath)
    files  = sorted([f for f in folder.rglob('*') if f.is_file()])

    if not files:
        print('no files found'); return

    total_orig = 0
    total_comp = 0
    file_nodes = []

    for fpath in files:
        with open(fpath, 'rb') as f:
            data = f.read()
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

    lines = [f'<?xml version="1.0" encoding="UTF-8"?>']
    lines.append(f'<svg xmlns="http://www.w3.org/2000/svg" xmlns:vault="urn:hilbert-vault" width="64" height="64">')
    lines.append(f'  <metadata>')
    lines.append(f'    <vault:folder')
    lines.append(f'      magic="{MAGIC}"')
    lines.append(f'      name="{folder.name}"')
    lines.append(f'      files="{len(file_nodes)}"')
    lines.append(f'      original_size="{total_orig}"')
    lines.append(f'      compressed_size="{total_comp}"')
    lines.append(f'      sha256="{folder_sha}"')
    lines.append(f'    />')
    for rel, orig, comp, sha, b64 in file_nodes:
        lines.append(f'    <vault:file name="{rel}" original_size="{orig}" compressed_size="{comp}" sha256="{sha}">')
        lines.append(f'      {b64}')
        lines.append(f'    </vault:file>')
    lines.append(f'  </metadata>')
    lines.append(f'  <rect width="64" height="64" fill="#0a0a0a"/>')
    lines.append(f'  <text x="32" y="32" text-anchor="middle" font-size="6" fill="#333">{folder.name}</text>')
    lines.append(f'  <text x="32" y="42" text-anchor="middle" font-size="5" fill="#222">{len(file_nodes)} files</text>')
    lines.append(f'</svg>')

    out = str(folder) + '.vault.svg'
    with open(out, 'w') as f:
        f.write('\n'.join(lines))

    svg_size = os.path.getsize(out)
    ratio    = svg_size / total_orig if total_orig else 0
    print(f'encoded  → {out}')
    print(f'files={len(file_nodes)}  original={total_orig}B  compressed={total_comp}B  svg={svg_size}B  ratio={ratio:.2f}x')
    print(f'folder_sha={folder_sha[:16]}...')

def decode(svgpath, outdir=None):
    with open(svgpath, 'r') as f:
        content = f.read()

    # parse via string search — bypass namespace issues
    def get_attr(tag, attr):
        idx = content.index(f'{attr}="', content.index(tag)) + len(f'{attr}="')
        return content[idx:content.index('"', idx)]

    folder_name = get_attr('vault:folder', 'name')
    n_files     = int(get_attr('vault:folder', 'files'))

    base = Path(outdir) if outdir else Path(svgpath).parent / (folder_name + '_decoded')
    base.mkdir(parents=True, exist_ok=True)

    # extract all vault:file blocks
    import re
    file_blocks = re.findall(r'<vault:file(.*?)>(.*?)</vault:file>', content, re.DOTALL)

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
        with open(outpath, 'wb') as f:
            f.write(data)

        status = '✓' if ok else '✗'
        print(f'  {status} {rel}  ({len(data)}B)')
        if ok: ok_count += 1

    print(f'\ndecoded → {base}')
    print(f'files={ok_count}/{n_files}  {"✓ ALL PASS" if ok_count == n_files else "✗ SOME FAILED"}')

def decode_unused(svgpath, outdir=None):
    with open(svgpath, 'r') as f:
        content = f.read()
    root = ET.fromstring(content)
    ns   = 'urn:hilbert-vault'
    meta = root.find('metadata')

    folder_node = meta.find(f'{{{ns}}}folder')
    folder_name = folder_node.attrib['name']
    n_files     = int(folder_node.attrib['files'])

    base = Path(outdir) if outdir else Path(svgpath).parent / (folder_name + '_decoded')
    base.mkdir(parents=True, exist_ok=True)



def verify(orig_folder, decoded_folder):
    orig    = Path(orig_folder)
    decoded = Path(decoded_folder)
    files   = sorted([f for f in orig.rglob('*') if f.is_file()])

    ok = 0
    for f in files:
        rel  = f.relative_to(orig)
        d    = decoded / rel
        if not d.exists():
            print(f'  ✗ MISSING {rel}'); continue
        ha = hashlib.sha256(f.read_bytes()).hexdigest()
        hb = hashlib.sha256(d.read_bytes()).hexdigest()
        match = ha == hb
        print(f'  {"✓" if match else "✗"} {rel}')
        if match: ok += 1

    print(f'\nverify: {ok}/{len(files)} identical {"✓ ALL PASS" if ok == len(files) else "✗ MISMATCH"}')

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
        print('  python hilbert_vault_v3.py encode <folder>')
        print('  python hilbert_vault_v3.py decode <folder.vault.svg> [output_dir]')
        print('  python hilbert_vault_v3.py verify <original_folder> <decoded_folder>')
