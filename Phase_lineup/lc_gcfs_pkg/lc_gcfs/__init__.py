"""
lc_gcfs — LetterCube Geometric Cube File Store
local org library

Quick start
-----------
from lc_gcfs import LCFile

f = LCFile.open("myfile", seeds=[0xDEADBEEF, 0xCAFEBABE])
data = f.read(0)          # bytes | None
seed = f.seed(0)          # int (uint64)
info = f.stat()           # dict
f.delete()                # triple-cut ghost delete

Palette
-------
from lc_gcfs import palette_set, palette_clear
palette_set(face=0, angle=128)

Optional server
---------------
from lc_gcfs.server import start
start(port=8766)
"""

from .lc_client import LCFile, palette_set, palette_clear, GCFS_PAYLOAD_BYTES

__all__ = ["LCFile", "palette_set", "palette_clear", "GCFS_PAYLOAD_BYTES"]
__version__ = "1.0.0"
