# GEMM graph-build corrections and candidate reduction

Base: `amd-cagra-submit` at `f6a8e1dc38f68c2a80ca76986e7607b222b9a341`.

This change keeps the existing 128-D GEMM build path, graph degree, search
settings, and cuVS brute-force fallback. The 256-D AUTO build is unchanged.

## Changes

- Offset base-vector norms by the current base tile in both the rocBLAS and
  WMMA epilogues. Previously, every tile used the first tile's norms, which
  changes nearest-neighbor ordering for data with nonuniform row norms.
- Restore query norms on the final top-k output. Selection can omit this
  per-row constant, but graph symmetrization compares distances across rows
  and requires complete squared L2 distances.
- Retain exactly `gd` candidates per tile and in the merge buffer. The old
  `gd+32` pool was truncated without reranking or recomputing distances, so
  it could not correct numerical error. Equal-distance neighbor IDs may
  still differ because select_k need not use a stable tie order.
- Select `min(gd, tile_rows)` on short tiles, and merge their actual lengths.
  Initialize missing entries in a short first tile to infinity. This avoids
  rejecting valid inputs or requesting more candidates than a tile contains.
- Release allocations and the invocation-owned rocBLAS handle on HIP errors.
  The prior early returns skipped cleanup; the shared handle could also be
  destroyed while another build still used it. Environment choices are now
  local to the invocation. Handle lifetime follows the
  [rocBLAS programming guide](https://rocm.docs.amd.com/projects/rocBLAS/en/latest/how-to/Programmers_Guide.html).
- Clamp scratch tile dimensions to the dataset size, guard unsupported
  dimensions, and use rocBLAS when a dimension is not a multiple of 16.
- Specify the offload architecture even without rocWMMA headers: norm and
  merge kernels still require device code. Existing gfx1100 fallback remains.
- Replace misleading component timers with one `total_wall` measurement of
  the GEMM helper, including allocation and cleanup. The old “gemm+norm” timer
  included selection and copying, while “select+merge” timed two adjacent
  events after the work had completed.

## Expected work reduction, not measured speedup

At the default degree 96, the candidate width changes from 128 to 96: 25%
fewer candidate entries selected, merged, and stored. At query tile 2048,
the four candidate arrays shrink from 4 MiB to 3 MiB. The large distance
scratch matrix and the number of GEMM operations do not shrink. The optional
distance restoration adds O(n * gd) work after selection, rather than adding
query norms to every O(n^2) pair. These counts are not a latency or score claim.

## Local validation

Run from the repository root on Linux:

```sh
python3 unittest/oblib/vector/hipvs_gemm/test_cpu.py
```

Eight tests pass using GCC 11 with AddressSanitizer and UndefinedBehaviorSanitizer:
single tile, nonuniform norms across tiles, one-row tail, tiles smaller than K,
all six allocation failure points, output-copy failure, select_k failure, and
headless target selection with/without WMMA headers. Before the relevant fixes,
the tests reproduced incorrect neighbors, missing query norms, rejection of
short tiles, leaked allocations/handles/events, and a missing offload target.

The harness compiles production host control flow and scalar kernels. Kernel
launches execute serially; the row reduction uses one thread. GPU library calls
use CPU doubles and fixtures contain small integers exactly representable in
fp16. This checks tiling, distances, bounds and cleanup, not fp16 encoding or
GPU execution. The build test runs the real shell script with a recording
compiler double.

No ROCm/HIP bridge build, real WMMA execution, asynchronous/concurrent GPU
validation, server integration suite, or contest evaluation was run locally.
GPU recall, build/search time, peak VRAM and score remain unmeasured. The user
will provide the external evaluation results; no parameter tuning is claimed
from these CPU tests. `SEEKDB_CUVS_BF_GEMM=0` retains the existing fallback for
a later controlled comparison.
