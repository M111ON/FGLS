# FGLS - Frustum Geometry Logic System

## Project Overview
FGLS is a specialized C-based logic system designed for geometric simulation and state tracking, specifically focusing on "Frustum Geometry," "Ghost Integration," and "Apex Activation." It employs advanced spatial mapping techniques such as Hilbert curves, Fibonacci harmonics, and radial routing (GeoNet).

The system is designed with strict architectural invariants:
- **No Heap Allocation:** All components are stack-allocated or use fixed-size pools to ensure deterministic behavior and performance.
- **Callback-Driven:** Integration is handled via callbacks to allow for streaming data processing (e.g., `scan_buf`).
- **Geometric Determinism:** Core state transitions and "ghost" derivations are pure functions based on geometric slope calculations.

## Architecture

### Core Components (`src/core/`)
- **GeoNet (`geo_net.h`):** The radial routing layer that maps values and addresses to spokes, slots, and mirror masks.
- **Geomatrix (`pogls_geomatrix.h`):** An 18-way signature gate system for packet validation using spatial locality (Hilbert blocks).
- **ThirdEye (`geo_thirdeye.h`):** An adaptive observer that tracks system state and manages "anomaly" signals.
- **Cylinder (`geo_cylinder.h`):** Provides the underlying spatial mapping for the radial coordinate system.

### Phase 3 Modules (`src/phase3/` and `phase3/`)
- **Apex Activation (`geo_apex_activation.h`):** Detects activation events when geometric slopes converge within Fibonacci harmonic bands.
- **Boundary Fabric (`geo_boundary_fabric.h`):** Manages the expansion of the geometric network boundaries.
- **Ghost Integration (`geo_ghost_integration.h`):** The primary pipeline that wires together scanners, watchers, and apex activation.
- **Expansion Topology (`geo_expansion_topology.h`):** Handles the logical structure of the expanding frustum space.

## Building and Running

### Prerequisites
- GCC or a compatible C compiler.

### Compilation
The project follows a header-only or header-heavy design where much of the logic is inlined. Tests are the primary entry points.

To compile a test (e.g., Apex Activation):
```bash
gcc -I./src -o test_apex_activation test_apex_activation.c
```

### Running Tests
After compilation, run the resulting binary:
```bash
./test_apex_activation
```

Existing tests include:
- `test_apex_activation.c`
- `test_boundary_fabric.c`
- `test_expansion_topology.c`
- `test_integration.c`
- `test_reconstruct_path.c`

## Development Conventions

### Naming Strategy
- `geo_*`: General geometric and system headers.
- `pogls_*`: Implementation files or specific logic modules (Frustum Geometry Logic System).

### Coding Style
- **Pure Functions:** Favor deterministic functions for state derivation (e.g., `apex_derive_child`).
- **Fixed Memory:** Avoid `malloc`/`free`. Use fixed-size arrays and `struct` contexts.
- **Inline Logic:** Many core algorithms are implemented in headers as `static inline` functions for performance and ease of integration.

### Testing Practices
- Tests use a simple `ASSERT` macro and track pass/fail counts.
- Empirical validation of harmonic bands and geometric convergence is central to the testing strategy.
