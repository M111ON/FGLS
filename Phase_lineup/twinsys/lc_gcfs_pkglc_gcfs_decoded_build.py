"""
_build.py — locates lc_api.so at runtime
called once by __init__.py; result is cached in module scope
"""
import ctypes
import os
import subprocess
import sys


def _find_so() -> str:
    """Return absolute path to lc_api.so, building if missing."""
    pkg_dir = os.path.dirname(__file__)
    so_path  = os.path.join(pkg_dir, "lc_api.so")

    if os.path.exists(so_path):
        return so_path

    # fallback: try to build on-the-fly from src/ next to package root
    src_dir = os.path.join(os.path.dirname(pkg_dir), "src")
    src_c   = os.path.join(src_dir, "lc_api.c")
    if os.path.exists(src_c):
        print("[lc_gcfs] lc_api.so not found — building now ...", file=sys.stderr)
        result = subprocess.run(
            ["gcc", "-O2", "-shared", "-fPIC",
             "-I", src_dir, "-o", so_path, src_c],
            capture_output=True, text=True,
        )
        if result.returncode == 0:
            return so_path
        raise RuntimeError(f"[lc_gcfs] build failed:\n{result.stderr}")

    raise FileNotFoundError(
        f"[lc_gcfs] lc_api.so not found at {so_path} and src/lc_api.c missing. "
        "Re-install with: pip install --force-reinstall ./lc_gcfs"
    )


def load_lib() -> ctypes.CDLL:
    so_path = _find_so()
    lib = ctypes.CDLL(so_path)

    # ── api_open ──
    lib.api_open.restype  = ctypes.c_int
    lib.api_open.argtypes = [
        ctypes.c_char_p,    # name
        ctypes.c_uint32,    # chunk_count
        ctypes.c_char_p,    # seeds_hex  (comma-separated)
    ]

    # ── api_read ──
    lib.api_read.restype  = ctypes.c_int
    lib.api_read.argtypes = [
        ctypes.c_int,                       # gfd
        ctypes.c_uint32,                    # chunk_idx
        ctypes.POINTER(ctypes.c_uint8),     # out_buf
        ctypes.c_uint32,                    # buf_size
    ]

    # ── api_delete ──
    lib.api_delete.restype  = ctypes.c_int
    lib.api_delete.argtypes = [ctypes.c_int]

    # ── api_rewind ──
    lib.api_rewind.restype  = ctypes.c_int
    lib.api_rewind.argtypes = [
        ctypes.c_int,
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_uint8),
    ]

    # ── api_seed ──
    lib.api_seed.restype  = ctypes.c_uint64
    lib.api_seed.argtypes = [ctypes.c_int, ctypes.c_uint32]

    # ── api_palette_set / api_palette_clear ──
    lib.api_palette_set.restype    = None
    lib.api_palette_set.argtypes   = [ctypes.c_uint8, ctypes.c_uint16]
    lib.api_palette_clear.restype  = None
    lib.api_palette_clear.argtypes = [ctypes.c_uint8, ctypes.c_uint16]

    # ── api_stat ──
    class _ApiStat(ctypes.Structure):
        _fields_ = [
            ("lc_fd",       ctypes.c_int),
            ("chunk_count", ctypes.c_uint32),
            ("ghosted",     ctypes.c_uint32),
            ("valid",       ctypes.c_uint32),
        ]
    lib._ApiStat = _ApiStat  # attach so callers can use it

    lib.api_stat.restype  = None
    lib.api_stat.argtypes = [ctypes.c_int, ctypes.POINTER(_ApiStat)]

    return lib
