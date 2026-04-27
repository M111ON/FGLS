#!/usr/bin/env python3
"""Test vault_auto integration with all codecs."""
import subprocess, tempfile, os, hashlib, sys

def test_roundtrip(enc_func, dec_func, orig_path, vault_path):
    orig = open(orig_path, 'rb').read()
    cwd = os.getcwd()
    r_enc = subprocess.run(enc_func, capture_output=True, text=True, cwd=cwd)
    if r_enc.returncode != 0:
        return False, f"encode failed: {r_enc.stderr}"
    vault_size = os.path.getsize(vault_path) if os.path.exists(vault_path) else 0
    os.rename(orig_path, orig_path + '.orig')
    r_dec = subprocess.run(dec_func, capture_output=True, text=True, cwd=cwd)
    dec_ok = r_dec.returncode == 0 and os.path.exists(orig_path)
    if dec_ok:
        dec = open(orig_path, 'rb').read()
        match = orig == dec
        os.remove(orig_path)
    else:
        match = False
    os.rename(orig_path + '.orig', orig_path)
    return match, f"size={len(orig)}B vault={vault_size}B encode_ok={r_enc.returncode==0} decode_ok={dec_ok}"

with tempfile.TemporaryDirectory() as tmp:
    print("=== vault_auto integration test ===\n")
    tests = [
        ('small.txt', b'abc' * 100),
        ('binary.bin', bytes(range(256)) * 20),
        ('empty.txt', b''),
    ]
    passed = 0
    for name, data in tests:
        path = os.path.join(tmp, name)
        with open(path, 'wb') as f:
            f.write(data)
        ok, msg = test_roundtrip(
            ['./vault_auto', 'enc', path],
            ['./vault_auto', 'dec', path + '.vault'],
            path, path + '.vault'
        )
        status = '✓' if ok else '✗'
        print(f'{status} {name}: {msg}')
        if ok:
            passed += 1
    print(f'\nResult: {passed}/{len(tests)} passed')
    sys.exit(0 if passed == len(tests) else 1)
