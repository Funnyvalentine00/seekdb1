# PR 30 target integration implementation plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Resolve PR 30 against 63cffcb, preserve target optimizations, validate CRC-32C and update the PR.
**Architecture:** Merge target history. Keep its CRC-32C writer/reader, hardware dispatch, query optimizations and profile/output code; retain bounded response-buffer reuse and remove the superseded IEEE CRC optimization. Update independent tests to CRC-32C without claiming legacy IEEE WAL compatibility.
**Tech Stack:** C++17, Python3, macOS compatibility builds, Linux GitHub Actions.
**Spec:** User request in this chat: preserve target optimizations, coordinate CRC-32C, run correctness before updating PR.

## Global constraints

- Reuse the current PR branch; no force push, no merge into the competition branch.
- Legion is unavailable by user confirmation. Run local checks and Linux/image CI instead; no performance acceptance.
- Prior Legion timings remain historical, unplugged and reference-only.

## Review focus

- Missing/retained JSON response content across small, error and large requests; profile stdout must match.
- CRC-32C known vectors, seeds, unaligned tails, software fallback and SSE4.2.
- Real target-to-candidate WAL bytes and bidirectional replay; damaged tails must stay rejected.
- 32/64/96/128-dimensional TopK uses every coordinate, including cached and uncached scans.
- Retained target filter/vector caches must pass transactions, recovery and memory gates.

## Tasks

- [x] Merge target, resolve runStdio with target profile/output and bounded response reuse; restore common.h to target.
- [x] Demonstrate old IEEE WAL verifier fails on CRC-32C; replace its oracle with independent Castagnoli CRC, update helper/CI tests and record negative controls.
- [x] Build exact target and integrated Mac compatibility binaries; run public SQL, regression suite, protocol/profile differential, WAL, fuzz/transaction and sanitizer checks.
- [ ] Run Linux/image correctness CI with actual target cross-version comparison and focused dimension/CRC tests; fix evidenced integration regressions while keeping target optimizations.
- [ ] Fresh whole-branch review, archive logs, commit/push and verify PR refs, conflict status and final CI; rewrite title/body for final scope.

## Rulings / execution ledger

- CRC: keep target CRC-32C unchanged and withdraw obsolete IEEE slicing-by-8, rather than introduce a second unmeasured CRC algorithm. Old IEEE-format WAL is not claimed compatible.
- Remote: user cannot power on Legion; do not retry it. macOS results are software/scalar only; Linux correctness must come from CI.

- Mac verification: ordinary and ASan/UBSan each21 checks passed; 2Linux-only skips each. CRC-32C4102-case units passed in both builds.

- Linux RED: run43732529292 caught 32-dimensional AVX2 Top1 returning id9 instead of id1. Root cause: target VECS==4 branches reduce only acc[0]. Ruling: retain batching/cache, remove incomplete special cases in all5 paths and use existing full reduction; required for correctness.
