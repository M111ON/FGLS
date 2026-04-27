"""
lc_client.py — Pythonic wrapper over lc_api.so (ctypes)
no network, no server — direct .so call every time
"""
from __future__ import annotations

import ctypes
from typing import List, Optional

from ._build import load_lib

GCFS_PAYLOAD_BYTES = 4608   # usable payload
GCFS_TOTAL_BYTES   = 4896   # header(288) + payload — api_rewind writes this size
GCFS_HEADER_BYTES  = 288

_lib = load_lib()            # loaded once per process


# ─────────────────────────────────────────────
#  LCFile  — one logical file in the LC store
# ─────────────────────────────────────────────
class LCFile:
    """
    Represents one open LC file (gfd = global file descriptor).

    Usage
    -----
    f = LCFile.open("doc42", seeds=[0xDEADBEEF, 0x12345678])
    data = f.read(0)          # bytes | None
    seed = f.seed(0)          # int
    f.delete()                # ghost all chunks
    """

    def __init__(self, gfd: int, chunk_count: int, name: str):
        if gfd < 0:
            raise RuntimeError(f"lcgw_open failed (gfd={gfd}) for '{name}'")
        self._gfd         = gfd
        self._chunk_count = chunk_count
        self._name        = name
        self._deleted     = False

    # ── factory ────────────────────────────────
    @classmethod
    def open(cls, name: str, seeds: List[int]) -> "LCFile":
        """
        Open / create a file.
        seeds  : list of uint64 — one per chunk (len == chunk_count)
        """
        chunk_count = len(seeds)
        seeds_hex   = ",".join(f"{s:x}" for s in seeds).encode()
        gfd = _lib.api_open(
            name.encode(),
            ctypes.c_uint32(chunk_count),
            seeds_hex,
        )
        return cls(gfd, chunk_count, name)

    # ── read ────────────────────────────────────
    def read(self, chunk_idx: int) -> Optional[bytes]:
        """
        Returns payload bytes for chunk_idx, or None if blocked/ghost.
        """
        self._check_open()
        buf = (ctypes.c_uint8 * GCFS_PAYLOAD_BYTES)()
        n   = _lib.api_read(
            self._gfd,
            ctypes.c_uint32(chunk_idx),
            buf,
            ctypes.c_uint32(GCFS_PAYLOAD_BYTES),
        )
        return bytes(buf[:n]) if n > 0 else None

    # ── rewind ──────────────────────────────────
    def rewind(self, chunk_idx: int) -> Optional[bytes]:
        """
        Reconstruct chunk from seed only (works even after delete).
        Returns bytes or None on failure.
        """
        buf = (ctypes.c_uint8 * GCFS_TOTAL_BYTES)()  # api_rewind writes 4896 bytes
        rc  = _lib.api_rewind(
            self._gfd,
            ctypes.c_uint32(chunk_idx),
            buf,
        )
        return bytes(buf[GCFS_HEADER_BYTES:GCFS_TOTAL_BYTES]) if rc == 0 else None

    # ── seed ────────────────────────────────────
    def seed(self, chunk_idx: int) -> int:
        """Return seed uint64 for chunk_idx."""
        self._check_open()
        return int(_lib.api_seed(self._gfd, ctypes.c_uint32(chunk_idx)))

    # ── delete ──────────────────────────────────
    def delete(self) -> int:
        """
        Triple-cut ghost delete.  Returns chunks_ghosted (>=0) or error code (<0).
        After delete the file handle is invalid for read; rewind still works.
        """
        self._check_open()
        rc = _lib.api_delete(self._gfd)
        self._deleted = True
        return rc

    # ── stat ────────────────────────────────────
    def stat(self) -> dict:
        """Return dict with lc_fd, chunk_count, ghosted, valid counts."""
        s = _lib._ApiStat()
        _lib.api_stat(self._gfd, ctypes.byref(s))
        return {
            "gfd":         self._gfd,
            "name":        self._name,
            "lc_fd":       s.lc_fd,
            "chunk_count": s.chunk_count,
            "ghosted":     s.ghosted,
            "valid":       s.valid,
        }

    # ── properties ──────────────────────────────
    @property
    def gfd(self) -> int:
        return self._gfd

    @property
    def chunk_count(self) -> int:
        return self._chunk_count

    @property
    def name(self) -> str:
        return self._name

    # ── internals ───────────────────────────────
    def _check_open(self):
        if self._deleted:
            raise RuntimeError(f"LCFile '{self._name}' has been deleted")

    def __repr__(self):
        return (f"LCFile(name={self._name!r}, gfd={self._gfd}, "
                f"chunks={self._chunk_count}, deleted={self._deleted})")


# ─────────────────────────────────────────────
#  Palette helpers  — module-level, not per-file
# ─────────────────────────────────────────────
def palette_set(face: int, angle: int) -> None:
    """Enable palette bit for (face 0-3, angle 0-511)."""
    _lib.api_palette_set(ctypes.c_uint8(face), ctypes.c_uint16(angle))


def palette_clear(face: int, angle: int) -> None:
    """Disable palette bit for (face 0-3, angle 0-511)."""
    _lib.api_palette_clear(ctypes.c_uint8(face), ctypes.c_uint16(angle))
