# GPU validation record

SQL final acceptance: full existing GPU smoke12/12 PASS; native equivalent of
Docker-only DDL restart smoke6/6 PASS. Additional tile_b128 coverage on10K data:
7/7 PASS, production fused_tiles312, direct GPU snapshot reload/no rebuild,
CPU reload and post-restart DML fallback. Actual process maps show final .so.
Native fused check initially looked at the startup console, while SeekDB had
redirected stderr to its application log; preserve that rejected run. Corrected
log oracle and a new database/evidence suffix pass. No candidate change or
additional formal performance group. Full source tree8866 files matches f97;
startup revision metadata is not used as proof (see final report).

Reference: `f97bc2823`; final implementation: E02 fused epilogue. Target: actual gfx1100/48GiB runtime-visible
device0, ROCm7.2.1. This is a new environment, not a replay of historical7.2.4 runs.

Passed: minimal HIP allocation/kernel; production bridge compile/dynamic symbols;
real norms/WMMA/rocBLAS/copy/merge; full GEMM C ABI with real cuVS select_k in ten
shape/path combinations. No invalid IDs, missing top-k entries, exact-distance
rank inversions or operand-bound failures in those fixtures. Standalone8192
distances: WMMA max absolute error .00390625; full-helper fixture max .0078125.
rocBLAS fixture error0. These finite tests do not prove general Recall/performance.

FP32 bridge lifecycle: build/query/reuse/save/load/free PASS, 512x128 integer
fixture,16 queries, @10 tie-aware Recall1.0, all self matches. FP16 lifecycle:
build/query/reuse/save/load/free PASS after the isolated hipVS descriptor patch.
The original failed FP16 snapshot loads unchanged after repair. Both float and
half reject wrong row counts/dimensions. Patch accepts e2 as well as f2; SHA256:
`f3eac7ccc8c000291b054d1f5e48bba51144394f37e3cb5d95e71f4b56aa571d`.

Final acceptance: CPU8/8, compile, ldd-r, standalone8192 fused/unfused distances
bitwise equal, actual linked helper20 combinations PASS. Runner checks positive
fused/WMMA/rocBLAS counters. It first rejected stale-library resolution via the
common SONAME; a private symlink/loader path now forces the requested candidate.
Preserve that failed log. Earlier helper coverage is superseded by this final run.

E02 groups1/2: all20 workers R@10=1/@100=.9933, sampled peak747110400B and6MiB
post-warmup-free growth. Every candidate build logs1830 fused tiles, success,
no fallback. Final default-on bridge repeats250K Recall/build/QPS/resources and
FP16/FP32/BF fallback lifecycles. 10K/256D AUTO R@10=1/@100=1.

Raw evidence remains in `/workspace/seekdb-amd-20261106/logs/` with rc files in
the task root and local evidence directory. Dependency hashes are recorded.
No SQL/contest performance qualification yet. VRAM peaks are sampled at2ms,
not guaranteed instantaneous high-water marks. 1M E02 group3 reference timed out
after240s before its first build returned; no candidate sample exists. Do not
infer1M gain/regression or root cause. After termination, small/250K final GPU
checks pass without a device reset.

Reproduction after dependency qualification:

```sh
HIPVS_ROOT=/workspace/seekdb-amd-20261106/hipvs-install \
  bash unittest/oblib/vector/hipvs_gemm/test_gpu.sh \
  /workspace/seekdb-amd-20261106/libseekdb_cuvs_bridge.so
```

The existing SQL smoke runner must use fresh isolated directories, evidence path,
ports and an observer built with `-DOB_BUILD_CUVS=ON` and trace support. Its
hardcoded library paths need an appropriate private-prefix runtime arrangement;
do not alter global `/opt/rocm` or reuse another project's observer as qualification.
