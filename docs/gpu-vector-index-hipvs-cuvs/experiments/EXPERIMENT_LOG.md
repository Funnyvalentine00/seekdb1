# Experiment and decision ledger

## Final acceptance rulings (supersede provisional notes below)

Final SQL: existing full GPU smoke12/12, native default DDL persistence6/6,
and forced-fusion tile_b128 persistence7/7 PASS. The extra geometry is small
functional coverage, not a new optimization or performance measurement. Its
first counter oracle checked the startup console rather than the application
log, so rejected an actual312-tile fused execution. Preserve that failed case;
corrected evidence lookup/new namespace passes. This final acceptance-tool
adaptation changes no GPU candidate and creates no fourth paired group.
Observer source8866 files match f97 Git blobs; two archive-excluded .gitignore
files are absent. Startup build metadata is stale and explicitly not provenance.
Snapshot graph6401713B reloads after restart with no GPU rebuild; actual process
maps confirm the final bridge. All task-owned live observers are stopped.

E02 groups1/2 complete: five pairs each, median build-latency reductions
18.268968% and18.107742% versus f97 GEMM, all10 pairs improve17.33-20.18%.
All20 workers R@10=1/@100=.9933, sampled VRAM747110400B, post-free growth6MiB.
Single-query QPS changes +.033%/+.329%, batch100 -.690%/-.139%: no query gain
claimed. An independent read-only reviewer recomputed raw metrics and verified
per-build1830 fused/1830 WMMA/138 BLAS tiles; no production blocker identified.

E02 group3 (last allowed group), n1M/d128, stopped at a240s reference first-build
timeout. Zero complete pairs and no candidate execution. Live telemetry showed
39W,31C edge/36C junction/30C memory; the100% utilization gauge is unreliable
even idle. No API stack was captured; exact stall location/root cause unknown.
Preserved partial files. Ruling: stop exploration and scaling, retain the
qualified250K result, spend remaining time on acceptance. Cost:1M/contest
performance remains unknown; no automatic fourth group or new hypothesis.

Validation repair batch1: enforce actual linked-library counters and bind the
benchmark main/monitor/bridge to visible device0 (record BDF, fail on monitor
errors). Batch2: those counters exposed a common-SONAME loader resolving the
old baseline in test_gpu.sh. Force a private canonical-name symlink to the
requested candidate and private loader path. Earlier20-helper candidate claims
are superseded: standalone fusion and ctypes paired runs were actual candidate,
but the first helper executable used reference. Final corrected20-helper run
PASS with positive fused/unfused/rocBLAS counters. Count these two batches
conservatively as E02's2/2 corrections; no kernel repair was needed.

Final acceptance: CPU8/8, compile, ldd-r, GPU kernels/helper, FP16/FP32 and
explicit BF fallback lifecycle, wrong-shape reload rejection PASS. Bridge hash
`5a207cffc72fb16a6203700022295aced725bfa95b07a36a1aaa784051136bef`.
Unpaired250K acceptance:1807.15ms, R10=1/R100=.9933, peak747110400B,
post-free growth6MiB, single QPS2261.67. Observer build was paused only for this
recheck and resumed in finally. 10K/256D control R10=1/R100=1, free growth0.
No new paired promotion group or independent performance experiment was added.

FP16 dependency compatibility repair is green: rebuild cuvs_c only, original
failed snapshot reloads unchanged, float control still passes. Patch hash is
recorded in GPU_VALIDATION.md; isolated runtime only, no global ROCm changes.

## E02 — fused WMMA norm epilogue (started after E01 group1)

Hypothesis: eliminate the separate global-memory norm epilogue on aligned WMMA
tiles by staging each 64x64 accumulator tile in 16 KiB shared memory, then
writing norm[b]-2*dot directly. Partial tiles retain the rocBLAS path. This
targets the 25.75% kernel-time epilogue share in the partial E01 profile.
Risk: shared-memory/barrier cost or occupancy loss can outweigh saved traffic.
Admission: CPU suite, HIP compile, fused/unfused distance oracle, helper shapes,
small-data Recall and memory lifetime. Promotion: >=3% paired warm-build gain
versus f97 GEMM, identical fixture/Recall and acceptable memory; require nonzero
fused tile counter. Rollback is the explicit flag set to 0 and preserved f97 .so.
Corrections: 0/2. Formal groups: 1/3 started. Red test: the new fused kernel invocation
fails to compile against the unchanged kernel, as expected (E02-red.log).
Green admission: CPU8/8, bridge compile, real GPU8192 fused/unfused distances
bitwise equal, and20 full-helper combinations PASS. n10000 Recall@10=1.0,
@100=.9994; max stored-distance error7.33e-5, post-free growth6MiB (below16MiB
admission cap). No per-index persistent allocation was added.

## E01 — complete, existing-path qualification

Two groups, five alternating pairs each, n250000/d128. Group1 median speedup
2.22394x (55.0348% latency reduction); group2 median2.19070x (54.3525%).
All20 workers: Recall@10=1.0, @100=.9933. Sampled VRAM peak683671552 bytes
fallback versus747110400 GEMM. This qualifies current f97 GEMM versus its
maintained fallback on this research fixture; it is not a new-code improvement
or SQL/contest result. No third group needed. Corrections0/2.
The observer build attempted during group1 failed immediately before compilation
because SQLite was missing. Group2 had no observer build activity. Profiling
stall did not recur in unprofiled measurements.

## Admission work (no independent performance experiment consumed)

2026-11-07: takeover audit completed. Source/remote match `f97bc2823`. Preserved
three untracked GPU test files and all unrelated FTS worktrees/assets.

Installed hipVS in the task's private prefix, after AMD wheel endpoint returned
403. Built official source instead. Matched ROCmDS-CMake release/rocmds-26.03
across dependencies, disabled unused Roaring test-data download and repeated
dependency updates. Installed matching DLPack headers, hipMM and logger into the
private prefix. No global ROCm or driver changes.

CPU suite: 8/8 PASS. Production bridge compile and `ldd -r`: PASS. Real GPU kernel
probe plus full GEMM helper: PASS, both paths, tails, short tiles and dim130.

Ruling: exact final-distance-relative tolerance alone is inappropriate for
expanded-L2 cancellation. Keep the observed errors, apply an operand-magnitude
FP32 bound and independently reject wrong top-k/rank inversions. Cost if wrong:
small distance errors could be underqualified on other data; full dataset Recall
and distance checks remain required, with no blanket accuracy claim.

Ruling: stable IDs are not required at equal-distance ties. Lifecycle fixtures
compare valid IDs, exact distance ranks, self matches and tie-aware recall.
Both initial/reused searches scored recall1.0; strict stable-ID check was removed
from the test oracle after it rejected a legitimate tie order. Cost if wrong:
distance-rank changes still fail; broader ANN Recall is separately measured.

Windows `core.autocrlf=true` changed the first archive's shell scripts to CRLF.
Re-exported using `git -c core.autocrlf=false archive`; production C/C++/build.sh
bytes match Git blobs. Preserved original failed build log. LF archive SHA256:
`0898a24319a49879ee8c4358831c8e2966118f0d22fa4d7cfb454f16de974f3e`.

Real lifecycle: FP32 build/search/reuse/save/reload/free PASS. FP16 build/search
PASS, reload FAIL: hipRAFT writes descriptor `<e2\0`; hipVS C reader only accepts
`f2`. Preserved failed snapshot and baseline bridge. Minimal dependency repair
and red→green test pending; this is a compatibility prerequisite, not a speedup.

Ruling: user fixed model/effort and continuous execution take precedence over
skill suggestions to switch reviewer models or request routine approvals. Use
the current model and preserve the user's evidence directory instead of deleting
the research ledger at finish. Cost: review strength is limited to the chosen
model; verification scope will be stated.

## E01 — admitted, profiling in progress

Hypothesis: current WMMA/GEMM graph construction is faster than the maintained
cuVS BF fallback on the actual gfx1100 device without Recall/resource regression.
Same source/dependency build and fixture/truth; `SEEKDB_CUVS_BF_GEMM=1` versus `0`.
No large-data result or improvement claim yet. Formal groups: 0/3. Corrections: 0/2.

Admission: existing smoke-generator fixture, seed1314, n10000/dim128/nq100,
both modes R@10=1.0 / R@100=.9994, stored-distance max error7.33e-5, HIP sync
and bounded post-free memory checks PASS. Prepare a scaled n250000 fixture
with the same generator definition and independent FP64 truth; hash all arrays.
Profile before formal paired runs. Current parameters remain production defaults
(tq2048/tb16384, gd96, bfknn, half, itopk512/search-width4), not tuned values.

Initial rocprofv3 HIP/kernel/copy trace was terminated after an initialization
stall (>180s). It completed kernel traces for two builds, but one hipFree spanned
191.6s: do not use the profiled wall time. Partial GPU kernel duration shares:
selection36.40%, norm epilogue25.75%, WMMA23.28%, merge10.70%. Trace includes3936
epilogues and3660 WMMA launches. This motivates investigating redundant global
distance traffic once E01 paired baseline is established. Formal E01 group1 is
now running without rocprofv3; at most five alternating pairs. No speedup claim yet.

Observer setup: copied immutable dependency contents into the private task tree;
downloaded the exact missing jemalloc5.3.1 package declared by source. CMake then
requires pinned Rust1.97.1/sql-nio. Toolchain bootstrap is ongoing; no SQL metrics
are qualified. Local verified upload copies (~2GB) have been removed; remote
packages and extracted reusable artifacts remain.
