"""
server.py — optional FastAPI REST layer
only needed if other projects want HTTP instead of direct import

Usage
-----
from lc_gcfs.server import start
start(port=8766)

# or CLI:
python -m lc_gcfs.server --port 8766
"""
from __future__ import annotations

import ctypes
import base64
from typing import List

try:
    from fastapi import FastAPI, HTTPException
    from pydantic import BaseModel
    import uvicorn
    _HAS_SERVER_DEPS = True
except ImportError:
    _HAS_SERVER_DEPS = False

from .lc_client import LCFile, palette_set, palette_clear, GCFS_PAYLOAD_BYTES


def _require_deps():
    if not _HAS_SERVER_DEPS:
        raise ImportError(
            "Server deps missing. Install with: pip install 'lc_gcfs[server]'"
        )


# ─── request models ───────────────────────────────────────────
class OpenRequest(BaseModel):
    name: str
    seeds: List[str]          # hex strings e.g. ["DEADBEEF", "12345678"]


class PaletteRequest(BaseModel):
    face: int                  # 0-3
    angle: int                 # 0-511


# ─── build app ────────────────────────────────────────────────
def build_app() -> "FastAPI":
    _require_deps()

    app = FastAPI(title="LC GCFS API", version="1.0")

    # in-process file table  { gfd: LCFile }
    _files: dict[int, LCFile] = {}

    @app.post("/file/open")
    def file_open(req: OpenRequest):
        seeds = [int(s, 16) for s in req.seeds]
        f = LCFile.open(req.name, seeds)
        _files[f.gfd] = f
        return {"gfd": f.gfd, "chunk_count": f.chunk_count}

    @app.get("/file/{gfd}/read/{chunk}")
    def file_read(gfd: int, chunk: int):
        f = _files.get(gfd)
        if f is None:
            raise HTTPException(404, "gfd not found")
        data = f.read(chunk)
        if data is None:
            raise HTTPException(410, "blocked or ghosted")
        return {"payload_b64": base64.b64encode(data).decode(),
                "bytes": len(data)}

    @app.delete("/file/{gfd}")
    def file_delete(gfd: int):
        f = _files.get(gfd)
        if f is None:
            raise HTTPException(404, "gfd not found")
        n = f.delete()
        _files.pop(gfd, None)
        return {"chunks_ghosted": n}

    @app.get("/file/{gfd}/rewind/{chunk}")
    def file_rewind(gfd: int, chunk: int):
        f = _files.get(gfd)
        if f is None:
            raise HTTPException(404, "gfd not found")
        data = f.rewind(chunk)
        if data is None:
            raise HTTPException(500, "rewind failed")
        return {"payload_b64": base64.b64encode(data).decode()}

    @app.get("/file/{gfd}/seed/{chunk}")
    def file_seed(gfd: int, chunk: int):
        f = _files.get(gfd)
        if f is None:
            raise HTTPException(404, "gfd not found")
        return {"seed": hex(f.seed(chunk))}

    @app.get("/file/{gfd}/stat")
    def file_stat(gfd: int):
        f = _files.get(gfd)
        if f is None:
            raise HTTPException(404, "gfd not found")
        return f.stat()

    @app.post("/palette/set")
    def pal_set(req: PaletteRequest):
        palette_set(req.face, req.angle)
        return {"ok": True}

    @app.post("/palette/clear")
    def pal_clear(req: PaletteRequest):
        palette_clear(req.face, req.angle)
        return {"ok": True}

    return app


# ─── programmatic start ───────────────────────────────────────
def start(host: str = "127.0.0.1", port: int = 8766, reload: bool = False):
    _require_deps()
    import uvicorn
    uvicorn.run("lc_gcfs.server:build_app", host=host, port=port,
                reload=reload, factory=True)


# ─── CLI  ─────────────────────────────────────────────────────
if __name__ == "__main__":
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8766)
    p.add_argument("--reload", action="store_true")
    args = p.parse_args()
    start(args.host, args.port, args.reload)
