# lc_gcfs

LetterCube Geometric Cube File Store — local org Python library.  
Direct ctypes binding to `lc_api.so`. No server needed.

## Install

```bash
# from local path (org-internal)
pip install ./lc_gcfs_pkg

# with optional REST server deps
pip install './lc_gcfs_pkg[server]'
```

Requires `gcc` — `.so` is compiled during install.

## Usage

```python
from lc_gcfs import LCFile

# open / create  (seeds = one uint64 per chunk)
f = LCFile.open("doc42", seeds=[0xDEADBEEF, 0xCAFEBABE])

# read chunk → bytes | None
data = f.read(chunk_idx=0)

# get seed for a chunk
s = f.seed(0)          # int

# reconstruct from seed only (works post-delete)
data = f.rewind(0)

# stats
print(f.stat())
# {'gfd': 0, 'name': 'doc42', 'lc_fd': ..., 'chunk_count': 2, 'ghosted': 0, 'valid': 2}

# triple-cut ghost delete
f.delete()
```

### Palette filter

```python
from lc_gcfs import palette_set, palette_clear
palette_set(face=0, angle=128)    # enable
palette_clear(face=0, angle=128)  # disable
```

### Optional REST server

```python
from lc_gcfs.server import start
start(port=8766)
```

or CLI:

```bash
python -m lc_gcfs.server --port 8766
```

Endpoints mirror the original `lc_api_server.py`:  
`POST /file/open` · `GET /file/{gfd}/read/{chunk}` · `DELETE /file/{gfd}`  
`GET /file/{gfd}/rewind/{chunk}` · `GET /file/{gfd}/seed/{chunk}` · `GET /file/{gfd}/stat`  
`POST /palette/set` · `POST /palette/clear`

## Package layout

```
lc_gcfs_pkg/
├── setup.py          ← compiles lc_api.so via gcc on install
├── src/              ← C sources (not installed, used at build time)
│   ├── lc_api.c
│   ├── lc_hdr.h
│   ├── lc_wire.h
│   ├── lc_fs.h
│   ├── lc_delete.h
│   └── lc_gcfs_wire.h
└── lc_gcfs/
    ├── __init__.py   ← public: LCFile, palette_set, palette_clear
    ├── _build.py     ← .so locator + ctypes signature setup
    ├── lc_client.py  ← LCFile class
    └── server.py     ← optional FastAPI layer
```
