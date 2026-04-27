# AGENTS.md — POGLS Repository Quick-Start

**Scope:** Three loosely coupled sub-projects under one repo. Treat each as separate build system.

---

## Repository Layout

```
twinsys/
├── FGLS/                    # Frustum Geometry Logic System (C only)
│   ├── src/core/            # core headers (geo_net.h, pogls_geomatrix.h, …)
│   ├── src/phase3/          # phase 3 modules (apex, boundary, ghost_integration)
│   ├── phase3/              # test binaries live here after compile
│   ├── test_*.c             # test entry points (compile → run)
│   └── verify_all.sh        # full FGLS verification (bash)
├── lc_gcfs_pkg/             # Python package with C extension (ctypes)
│   ├── lc_gcfs/             # Python runtime (LCFile, server.py)
│   ├── src/                 # C source → lc_api.so (built at pip install)
│   └── setup.py             # gcc -shared -fPIC compile step
└── TPOGLS_core/             # main POGLS system (C, CUDA, Python glue)
    ├── core/                # primary C headers & implementation
    ├── gpu/                 # CUDA kernels and GPU-specific headers
    ├── test/                # C test files (compile manually)
    ├── rest_server_*.py     # versioned REST servers (S33–S58, S11)
    ├── pogls_cli.py         # v4 CLI (GPKT encode/decode)
    ├── Makefile             # builds pogls_runner (needs llama.cpp)
    └── TPOGLS_S36_Release/  # S33–S36 REST servers + handoffs
```

---

## Prerequisites

- **gcc** — required for all C compilation (FGLS, lc_gcfs, TPOGLS tests)
- **Python 3.9+** — for lc_gcfs_pkg and server scripts
- **CUDA toolkit** — required if building/running GPU code (nvcc, located via `nvcc --version`)
- **llama.cpp** — required for `make` in TPOGLS_core; build llama.cpp first, note its path

On Windows: use Git Bash or WSL for `./verify_all.sh` and other bash scripts.

---

## FGLS (Frustum Geometry Logic System)

### Compile a single test
```bash
gcc -I./src -I./src/core -I./src/phase3 -I. -o phase3/test_apex_activation test_apex_activation.c src/core/angular_mapper_v36.c
```

Include order matters: `-I./src` picks up `phase3/` and `core/` headers.

### Run all FGLS tests
```bash
cd FGLS
./verify_all.sh            # each step greps for "PASS"; exits non-zero on failure
```
Script compiles integration, apex, boundary, reconstruction, and runs the recursive expansion benchmark.

### Notes
- Tests are **standalone binaries** — no test framework, they print `[PASS]`/`[FAIL]`.
- Benchmark `bench_recursive_expansion` is pre-built; just `./` it.
- Header-heavy design — most logic is `static inline` in headers.

---

## lc_gcfs_pkg (LetterCube Geometric Cube File Store)

### Install (compiles C extension)
```bash
pip install ./lc_gcfs_pkg          # builds lc_api.so via gcc, places in package dir
```
Or editable: `pip install -e ./lc_gcfs_pkg`

Build is handled by `setup.py` → `BuildWithSO` class; `gcc -O2 -shared -fPIC -I./src -o lc_api.so src/lc_api.c`.

### Optional server extras
```bash
pip install './lc_gcfs_pkg[server]'   # FastAPI + uvicorn
```

### Run REST server
```bash
python -m lc_gcfs.server --port 8766
# or
from lc_gcfs.server import start; start(port=8766)
```
Endpoints: `POST /file/open`, `GET /file/{gfd}/read/{chunk}`, `DELETE /file/{gfd}`, `GET /file/{gfd}/rewind/{chunk}`, `GET /file/{gfd}/seed/{chunk}`, `GET /file/{gfd}/stat`, `POST /palette/set`, `POST /palette/clear`.

### Python usage
```python
from lc_gcfs import LCFile, palette_set, palette_clear
f = LCFile.open("doc42", seeds=[0xDEADBEEF, 0xCAFEBABE])
data = f.read(chunk_idx=0)
f.delete()
```

---

## TPOGLS_core (Main POGLS System)

### Build pogls_runner (llama.cpp integration)
```bash
# 1. Build llama.cpp first (output: build/libllama.so / llama.dll)
# 2. Then:
make LLAMA_DIR=/full/path/to/llama.cpp
```
Produces `pogls_runner`. Links against `libllama` and `libggml` with rpath set to `$(LLAMA_DIR)/build`. Preflight test target (`make preflight_test`) builds `gguf_midx_test` without llama linkage.

### Compile a C test manually
```bash
gcc -O2 -std=c11 -I. -o test_roundtrip test_roundtrip.c
# Many tests need multiple core headers included; they already #include what they need.
```

### CUDA tests
```bash
nvcc -o test_gpu_s17 test_gpu_s17.cu -I. -L. -l<deps>
```
GPU benchmarks live in `gpu/benchmark/`; compile with `nvcc`.

### Python components
- `pogls_cli.py` — v4 CLI for encode/decode/verify (talks to server at `POGLS_HOST:PORT`)
- `rest_server_s*.py` — versioned servers (S33, S34, S35, S36 in `TPOGLS_S36_Release/`; S11 in `TPOGLS_s11/`; S40, S41, S52, S54, S58 at root). Each exposes slightly different API shapes.

---

## Critical Conventions (Enforced by Code)

- **No heap allocation** in FGLS core modules — all buffers are stack or fixed pools. `malloc`/`free` prohibited in `core/` and `phase3/`.
- **Integer-only arithmetic** — no floating point in geometric core. Use fixed-point or scaled integers.
- **PHI constants** — only from `pogls_platform.h` (e.g., `PHI_UP`, `PHI_DOWN`). Do not hard-code sacred numbers.
- **QRPN guard** — every chunk must pass `pogls_qrpn_phaseE` verify before storage/output.
- **DiamondBlock alignment** — chunk size = 64 bytes exactly. All buffers and offsets must align.
- **GPU commit path forbidden** — commit/federation logic stays on CPU. GPU only for compute kernels.

---

## Architecture Anchors

- `pogls_delta.*` — delta encoding storage layer (already built, frozen)
- `pogls_twin_bridge.h` — twin-state reconciliation; used by roundtrip tests
- `geo_net.h` / `_geo_net.h` — radial routing (spoke/slot/mirror) — core addressing logic
- `pogls_platform.h` — system-wide constants (PHI, DiamondBlock size, QRPN params)
- `pogls_fold.*` — data folding/compression pipeline
- `pogls_hydra*` — multi-window scheduler and execution windows
- `pogls_snapshot.*` — admin snapshot/checkpoint capability
- `libpogls.so` / `libpogls_v4.so` — built shared library (found at repo root and in `TPOGLS_S36_Release/`)

---

## Gotchas & Non-Obvious Steps

1. **Include paths for FGLS tests** — must specify `-I./src -I./src/core -I./src/phase3 -I.` or headers won't resolve. See `verify_all.sh` line 4.
2. **Windows path formats** — `LLAMA_DIR` in Makefile uses forward-slash paths even on Windows. Use `/c/llama/llama.cpp` or absolute Windows path quoted properly.
3. **CUDA not on PATH?** — some `bench_*.cu` files require manual `nvcc` flags. Check `gpu/` for specific dependencies.
4. **Python .so location** — `setup.py` copies `lc_api.so` into `lc_gcfs/` package dir. If you rebuild manually, copy it there or imports fail.
5. **Versioned REST servers** — different endpoints across versions. `rest_server_s54.py` and `rest_server_s58.py` are the most recent at root; `TPOGLS_S36_Release/` contains S33–S36. Use matching client.
6. **verify_all.sh requires bash** — PowerShell `bash` command not available by default. Use Git Bash/WSL.
7. **PHI constants lock** — if you need PHI values, include `pogls_platform.h` and use `PHI_UP` / `PHI_DOWN`. Do not define locally.
8. **No test automation** — there is no `pytest` or `ctest`. Run binaries directly or call Python scripts. Document test names in handoff notes (S83, S36, etc.) if adding new suites.

---

## Handoff Docs (Must-Read for Context)

- `TPOGLS_core/project outline.md` — master plan, compression strategy, build order (S1–S8)
- `TPOGLS_core/S83_HANDOFF_v2.md` — weight streaming to llama.cpp, ModelIndex/WeightStream design
- `TPOGLS_core/TPOGLS_S36_Release/S3{3,4,5,6}_HANDOFF.md` — REST server evolution
- `FGLS/GEMINI.md` — FGLS architectural invariants and component map
- `TPOGLS_core/docs/HANDOFF_S13.md`, `HANDOFF_S14.md` — earlier phase handoffs

---

## Quick Reference Commands

| Subsystem | Build | Test | Server |
|-----------|-------|------|--------|
| FGLS | `gcc -I./src … -o test_name test_name.c src/core/angular_mapper_v36.c` | `./test_name` (binary) | N/A |
| lc_gcfs | `pip install ./lc_gcfs_pkg` | Python scripts in root (`test_s11_endpoints.py`, etc.) | `python -m lc_gcfs.server` |
| TPOGLS_core | `make LLAMA_DIR=…` (pogls_runner) | `gcc -o test_foo test_foo.c` (in `test/`) | `python rest_server_s54.py` |

---

## Branch/Release Context (Observed)

- Handoff docs tagged by sprint (S11, S13–S14, S33–S36, S40, S41, S52, S54, S58, S83).
- REST server filenames encode version; newest at repo root (S54, S58). Older versions kept in `TPOGLS_S36_Release/` for backward compatibility.
- No `git` history in working tree; rely on handoff docs for design decisions.

---

## Environment Variables

| Variable | Purpose | Default |
|----------|---------|---------|
| `LLAMA_DIR` | Path to llama.cpp for `make` in TPOGLS_core | `../llama.cpp` |
| `POGLS_HOST` | CLI/server host | `localhost` |
| `POGLS_PORT` | CLI/server port | `8765` (v4), `8766` (lc_gcfs server) |
| `POGLS_CTX` | Context ID for server ops | `default` |
| `POGLS_LIB` | Path to `libpogls_v4.so` for CLI | `/mnt/c/TPOGLS/libpogls_v4.so` |

---

## When in Doubt

1. Read the handoff doc closest to the component you're modifying (S83 for streaming, S36 for REST, GEMINI for FGLS).
2. Compile with `-I.` and the specific submodule `src/` paths shown in `verify_all.sh` or test files.
3. Do not refactor core headers (`core/`, `geo_*.h`, `pogls_*.h`) without checking handoff "Frozen Rules".
4. Keep GPU code in `gpu/` separated from commit path — federation and snapshot must remain CPU.
