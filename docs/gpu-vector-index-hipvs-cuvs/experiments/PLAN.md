# AMD CAGRA bounded research implementation plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task by task. The user authorized continuous inline execution; do not ask for routine plan or experiment approvals.

**Bounded closure:** Two hypotheses only. Checked tasks mean completed within the declared scope; partial profiling and unmeasured SQL/contest/1M performance are explicitly documented. No automatic continuation.

**Goal:** Find and retain a real, repeatable GPU build/query improvement while preserving Recall, correctness, resources and C ABI within the experiment budget.

**Architecture:** Protect the current AMD worktree and qualified reference binaries. Qualify the actual dependency/runtime combination, profile the existing bridge and SQL pipeline, then run small isolated A/B candidates. Promote only a measured winner; preserve failures and the final acceptance window.

**Tech Stack:** SeekDB C++/CMake, HIP/ROCm, hipVS/cuVS/CAGRA, rocBLAS, rocWMMA, Python/C++ regression and benchmark fixtures.

**Spec:** `MISSION.txt`; latest overriding constraints in `BUDGET.md`.

## Global constraints

- At most 6 independent GPU optimization experiments, 2 implementation corrections per experiment, 3 formal paired measurement groups per valid candidate.
- Default primary performance promotion gate: at least 3%, above noise, with original Recall@10/@100 gates and acceptable VRAM/stability.
- Final 25% of task window reserved for acceptance/delivery. Stop after delivery.
- No FTS changes, destructive Git actions, force push, automatic merge, driver/global ROCm modification, dataset deletion or overwritten evidence.
- Preserve fallback and active-path evidence. CPU verified, GPU compiled, GPU executed, GPU benchmarked and contest evaluated are separate levels.

## Review focus

- Current hipVS FP16 descriptor writer/reader disagreement; saved half and float indexes must reload, with wrong-shape files rejected.
- Candidate selection ties and cancellation error; use independent squared-distance/rank oracles, not stable-ID assumptions.
- Tail/small tiles and non-multiple-of-16 dimensions; compare actual top-k on both GEMM paths.
- Allocation/synchronization failure and concurrent ownership; CPU injection plus bounded real GPU resource checks.
- SQL fallback/freshness/restart and hidden GPUs; only the existing isolated observer smoke can qualify these contracts.

## Tasks

### Task 1: Qualify the current runtime and preserve the reference

Files: `STATE.md`, `GPU_VALIDATION.md`, `REGRESSION_REPORT.md`, dependency patch; GPU regression sources.

Interfaces: production `sk_bf_gemm_topk128(...)`, bridge build/search/free/save/load C ABI; artifacts keyed by source/dependency SHA and patch hash.

- [x] Read-only Git/source/assets audit and real GPU/toolchain identification.
- [x] Run CPU suite (8 PASS), build production bridge, check dynamic symbols, run real full-helper GPU regression (10 combinations PASS).
- [x] Reproduce FP16 reload failure; FP32 control passes. Preserve baseline bridge and failed snapshot.
- [x] Apply minimal isolated dependency descriptor compatibility patch, verify the preserved FP16 file loads and existing float path still passes; retain patch/logs.
- [x] Obtain isolated AMD observer, run existing GPU smoke and persistence checks where possible. Record concrete blockers and continue independent work if unavailable.
- [x] Commit reproducible verification assets and evidence, without claiming performance promotion.

### Task 2: Establish baseline and run E01

Files: benchmark fixture under this directory, `BASELINE.md`, `EXPERIMENT_LOG.md`, immutable raw runs.

Interfaces: same dataset/truth SHA and parameters for GEMM and fallback; JSONL raw samples and environment telemetry.

- [x] Recover existing datasets/benchmark assets; otherwise generate a fixed-seed, hashed 128D/256D research fixture with exact @10/@100 truth.
- [x] Admit only CPU/GPU-correct builds, check HIP status and VRAM release.
- [x] Profile build/search, allocations/transfers and CPU finalization; distinguish GEMM success, cuVS BF and VSAG routes.
- [x] E01: test whether current GEMM improves build time versus cuVS fallback on the actual GPU. Use no more than 3 formal groups, at least 5 alternating pairs/group, retaining raw samples, Recall and telemetry. Keep cold initialization separate.
- [x] Set a current validated reference only for the scopes actually qualified. Report missing SQL/contest metrics explicitly.

### Task 3: Bounded profile-driven candidate

Files: only the measured bottleneck's existing source file(s), relevant regression, per-experiment patch/log.

Interfaces: unchanged C ABI and fallback contracts; all experiments use the same reference and fixture hashes.

- [x] Before each new hypothesis, check counters, corrections, no-gain streak and remaining acceptance time in `BUDGET.md`.
- [x] Select from `NEXT_STEPS.md` using actual profiling. Record hypothesis, expected benefit/risk, exact target files, test, measurement and rollback in the experiment ledger before implementation.
- [x] Write/reproduce the relevant failing correctness or performance condition; make one minimal candidate and pass admission checks.
- [x] Measure within 3 groups/candidate; allow at most 2 implementation corrections. Promote only ≥3% repeatable primary gain above noise with Recall/resource gates, or label a smaller correctness/memory/stability benefit auxiliary.
- [x] Preserve accepted changes in independent commits; discard only the current experiment's tracked changes through a scoped inverse patch, retaining its raw evidence.
- [x] Reassess after 2 no-gain experiments; stop exploration after 3, budget exhaustion, instability, low expected value or deadline.

### Task 4: Final acceptance and delivery

Files: final reports, raw-result manifest, final commits.

- [x] Enter acceptance by the deadline; start no new large candidate.
- [x] Compile final bridge, rerun CPU and real GPU regression, fallback, Recall@10/@100, build/QPS and VRAM release checks. Run full SQL GPU smoke when an observer is available.
- [x] Verify actual optimized path, inspect final diff/ABI, review independently using the same user-fixed model if a reviewer is used; do not change models/effort.
- [x] Record all experiment counts/outcomes, measured gains/noise, best SHA, acceptance level, missing metrics/directions and stop reason. Normal push only; no automatic merge.
- [x] Deliver and stop. Do not reopen experiments after delivery.
