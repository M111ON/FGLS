# Current Stack

`phase11/` is the current late-stage working snapshot for this repository.

## Canonical module order

1. `geo_temporal_ring.h`
2. `geo_tring_stream.h`
3. `geo_pyramid.h`
4. `geo_fec.h`
5. `geo_rewind.h`
6. `geo_fec_rs.h`
7. `geo_error.h`
8. `geo_ops_surface.h`
9. `geo_cube_file_store.h`
10. `geo_kernel.cu`

## Practical split

### Most ready for service integration

- TRing ingest and ordering
- pyramid completeness checks
- rewind stats
- XOR / RS / hybrid recovery
- invariant and checksum status
- operational monitor snapshots via `geo_ops_surface.h`

### Still not fully sealed

- some historical references are still unresolved:
  - `pogls_chunk_index.h`

### Internal core now owned locally

- `Asset_core/` is now treated as internal system core for `phase11/`
- compatibility bridges exist in `phase11/` and `phase11/core/`

## Recommendation

If you are wiring external monitor/control first, build on:

- `geo_ops_surface.h`
- `geo_temporal_ring.h`
- `geo_tring_stream.h`
- `geo_fec.h`
- `geo_fec_rs.h`
- `geo_rewind.h`
- `geo_error.h`

Then restore the missing upstream cube/GPU dependencies as a separate step.
