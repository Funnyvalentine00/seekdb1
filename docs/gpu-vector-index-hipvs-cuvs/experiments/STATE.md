# AMD CAGRA research state

Updated: 2026-11-07 (Asia/Shanghai). Bounded work complete; no automatic next round.

## Protected state and takeover audit

- Repository: `Funnyvalentine00/seekdb1`; working branch `codex/amd-cagra-fixes-20261106`.
- Takeover local HEAD/live remote: `f97bc2823f3ff74c9a9ecbb232f82f1997cb90ed`.
- Cached `origin/amd-cagra-submit` is stale at `f6a8e1dc3`; live `git ls-remote` confirmed `f97bc2823`.
- The working branch has no configured upstream. No reset, clean, rebase or force push was performed during takeover.
- Three pre-existing untracked GPU regression files belong to this run; preserve them.
- AMD worktree: sibling `amd-cagra-opt`. The separate `seekdb1` FTS checkout and other WSL builds remain untouched.
- Reviewed Git branches, worktrees, log/reflog, current GPU docs, both existing smoke runners, bridge/GEMM source and build scripts, adaptor registration/search/freshness/snapshot/fallback paths, plugin opt-in, DDL validation and batch SQL implementation.
- README limitations lag current source: source already implements GPU snapshot persistence and default build/search parameter changes. Historical commit measurements are claims pending raw-evidence recovery/reproduction.
- No contest datasets, AMD observer binary or AMD benchmark logs were found in this worktree. Existing sibling artifacts/snapshots belong to FTS/File SQL and are preserved.

## Validation state

| Level | Current evidence |
| --- | --- |
| CPU verified | 8 production-control-flow tests, ASan/UBSan, remote GCC; PASS |
| GPU compiled | Production two-unit bridge, ROCm 7.2.1 / gfx1100; PASS |
| GPU executed | Final linked bridge: fused/unfused WMMA and rocBLAS,20 shapes/path combinations; PASS |
| GPU lifecycle | FP32/FP16 build/search/reuse/save/reload/free + wrong-shape rejection PASS; requires recorded private hipVS patch |
| GPU benchmarked | E01 GEMM vs fallback:54-55% lower build latency; E02 fusion vs f97 GEMM:18.11-18.27%, two5-pair groups |
| SQL integration | Full GPU smoke12/12; default restart6/6; forced-fusion SQL restart7/7 PASS. No SQL performance claim |
| Contest evaluated | No evaluation of this candidate supplied |

## Environment and artifacts

- Remote task root: `/workspace/seekdb-amd-20261106`; exact LF source export: `source-f97bc2823-lf/`.
- GPU: runtime-visible device 0, `AMD Radeon Graphics`, gfx1100, 48 GiB VRAM, BDF `0000:63:00.0`, NUMA node 0, PCIe 16 GT/s x16. Marketing model not independently established.
- Host: AMD EPYC 9334, two NUMA nodes, distance 10 local / 32 remote. Container quota: 16 CPU, 55 GiB RAM.
- ROCm 7.2.1; HIP 7.2.53211 / clang 22; rocBLAS 5.2.0; rocWMMA 2.2.0. This differs from historical ROCm 7.2.4 evidence.
- Isolated CMake 3.31.10 and hipVS 1.0 source build; global driver and ROCm installation unchanged.
- hipVS `f96a663d0a4441d6f5344bd3ef552b71ae7b370b`; hipRAFT `baafb19bb11d7a32060a46c66c6d440cac21b5c1`; hipMM `22732e49aa00ba179847e3b314371f2307f47a9a`; ROCmDS-CMake `58b184f87d98d5e68cc48863477a6b18281ebd4e`.
- Baseline bridge retained as `libseekdb_cuvs_bridge-f97bc2823.so`, SHA256 `38f54278afaaf14781e01b2d9c9bf4355eb3506f6e4e1c9d0a6e7c5114cb6792`.
- Raw logs, dependency commits, configuration/build commands and exit-code files are in the remote task root. Copy them into a local immutable evidence run before promoting a candidate.
- Local temporary CMake wheel, CPM helper and first archive were checksum-verified then removed after upload/use. Keep source, tests, logs, datasets and reusable builds.

## Next action

Best code: `6062f81e241ba39e34f75487f94dc89be29f9e31`. Preserve evidence,
commit/push final report and deliver. Do not start a third hypothesis.
1M reference timeout is unresolved; no scale-up claim. Final bridge SHA256:
`5a207cffc72fb16a6203700022295aced725bfa95b07a36a1aaa784051136bef`.
Final unpaired250K/d128 acceptance:1807.15ms, R@10=1/@100=.9933,
sampled peak747110400B, post-free growth6MiB. 10K/d256 AUTO control:
R@10=1/@100=1, no post-free growth. These do not replace SQL/contest evaluation.

SQL functional acceptance is complete:12/12 integration,6/6 default restart,
7/7 tile128 fused restart. Official contest and comparative SQL performance
remain unmeasured. Final resource check: no live owned observers/benchmarks,
both test ports closed, VRAM26759168B. Raw evidence is locally hash-verified.
