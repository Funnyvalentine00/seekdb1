# FTS V5 independent review handoff

本文件是供后续独立模型审查的最小证据包，不是已获得独立审查批准。本轮模型/推理配置固定，未自行切换模型。新实验已停止；不要自动重启任务。

## 1. Outcome and protected versions

- Original working baseline: feat/fts-opt-v4 @ f5fe975382f4b151f7181a70d7f7e05ff9b234fd; product 194150497aca9a69e395317bd6d5a64f26340a61.
- V5 isolated branch: feat/fts-opt-v5, starting protection/document commit 2e8d969ca; full parent SHA in v5-20261107/FINAL_STATE.json.
- No product optimization promoted. Full-gate BEST_VALIDATED remains unset; retained reference is 194150497, not an assertion that old V4 gates passed.
- Only E01 was implemented, with zero implementation corrections and exactly three formal AB pairs. Local and remote source were restored from recorded preimage after checking exact before/after SHA. No reset/clean or historical rollback.
- Original v4's two macOS compatibility changes remain untouched, and v3 workspace was not modified. Existing H2/H3/H7/JSON/boundary fixes are preserved.

## 2. Candidate and exact question

E01 changes only src/storage/fts/ob_fts_stop_word.cpp: replace get_refactored + set_refactored(overwrite) with one set_or_update operation. The anonymous updater assigns entry.first=word and increments entry.second by word_freq. Initial insertion remains count 1; invalid/non-grouping branches remain unchanged.

Source patch: v5-20261107/e01.patch, SHA256 82f85f8cb1b0ac33968762022b764ab0542a0aa642189d83ccb958dc31de3f41.
Source before SHA256: 467903a9f748d2e7e31aee0840a0cabddfba01932ef8f08ece82a80ac6967009.
Source after SHA256: 01c4e435c417cd8d7727f226a6f856d6b22b9003a25c106ab053a405d7cfeb7c.
Reference binary: 9544ca38c4d6cc336adb079f362eff1fa220739d58ce70bc2d0f1d948d5ade32.
Candidate binary: eff02f8fc44b0245ab33ddc54c61d002d854a77531014d52fa27f992f69e9e8d.

Review questions:

1. Does the updater exactly preserve overwrite semantics, especially collation-equal last spelling, count initialization and word_freq accumulation? Trace ObFTWord equality/hash and ObHashTable::set_or_update's synchronous bucket-locked callback.
2. Is referencing the current arena-owned ObFTWord only during the synchronous callback sufficient for ownership safety? No new zero-copy or cached token ownership was introduced.
3. Is the observed Beng per-statement tail improvement causal, given that SELECT1 tails also shifted and formal tokenization results reverse direction? Do not infer a qualified gain from favorable diagnostics.
4. Was early stopping justified by the recorded noise, rather than ordinary compilation/test failure? Should this candidate be revisited only in a demonstrably stable environment?

## 3. Measurements and decision

Reference and candidate use identical Clang17/CMake RelWithDebInfo (-O2 -g -DNDEBUG), two build jobs and lld threads=2. Controlled runtime: server four guest CPUs, client fifth guest CPU, cpu_count=4, memory_budget=2G. This protocol is separate from the earlier V4 six-pair window.

Official workload is unchanged: ROWS20000 BATCH500 ROUNDS3000 QUERY_ROUNDS200 SAMPLES3 WARMUP30 SKIP_LOAD0. Formal order AB/BA/AB; no fourth pair. Each formal run additionally records 1000 per-statement samples for seven workloads after official timing, with benchmark RSS captured before that diagnostic.

Tokenize IK formal median improvement +0.89%; Beng +20.05/-5.36/-6.87%, median -5.36%. Query CN median -1.75%. Beng P95/P99 diagnostic medians improve 19.05/15.62%, but SELECT1 P95 also improves 12.73%. Benchmark RSS median changes -0.92%. Report all these together; do not cherry-pick the favorable tails or treat RSS as a proven allocator fix.

Two AA protocols show large control/tokenize variability. One bounded per-worker schedstat probe preserves all 9000 latency samples and marks a worker-set-changing CPU observation incomparable. It is not Linux hardware instruction counting.

Decision: FAIL_TO_PROMOTE; candidate source removed, patch/binary/data retained. User early-stop condition 4 triggered: performance environment did not reliably resolve the expected small effects within the permitted protocols. Budget 8, actual 1, success 0, failed-to-promote 1, partial 0. E02 and remaining queue were not started. No repeated runs to chase a passing result.

## 4. Correctness and limitations

Before, candidate and final retained reference each execute FTS13/15, FileSQL1/1, AI5/5: total57 pass,6 pre-existing failures,0 skipped. FTS basic_dml and partitioned_simple_query failure bodies match across all three runs, excluding only timestamp/file headers. Existing failures are not waived as full acceptance.

Each version has nine successful MATCH boundaries, 22 original Tokenize per-statement comparisons, 12 extra aggregation probes, DML/6-decimal scores/prepared statements and 80 concurrent requests. Outputs/errors/statuses match. In the extra probes NULL and REPEAT-generated LOB retain baseline errors; do not call every SQL statement successful.

Official scoring program ran on all six formal reports plus final reference, locally saturated at100. No official platform CI, Linux perf counters, ASan or TSan run. Differential tests do not prove complete memory safety or arbitrary long-query behavior.

## 5. Artifacts and resume hazards

Read OPTIMIZATION_REPORT.md, REGRESSION_REPORT.md, BASELINE.md, EXPERIMENT_LOG.md, and v5-20261107/{FINAL_STATE.json,BUDGET.json,e01-analysis.json}. Archive hashes are in archive-manifest.json. Full measured/test output and scripts are in final-evidence.tar.gz; archived preimage is e01-preimage.txt. Full server internal logs and process stdout remain on the node with18 preserved V5 data directories, outside the compact archive.

Node: /home/xiazihan/benchmark/fts-v4-integration-20261106. Use saved-binaries/seekdb-fixed for the retained reference. src/build_release/src/observer/seekdb is still the E01 cache output; restored source does not turn it into the original reference. Candidate sealed at v5/saved-binaries/seekdb-e01. Do not rerun one-shot drivers: tags are exclusive, and build_candidate.py expects source before the patch.

Harness notes: the first correctness attempt stopped at an overly strict bind-based port preflight before creating an instance; control_v2 uses connect_ex and preserves the failure log. The reused validate_source.py refreshed logs/source-fixed.json with identical bytes (SHA25685a27f88bd681b641dcb0d94e4af1ede26e6b565bae4bc9aa30e5958e9af0656); no historical benchmark data changed. Future validators should write to a new experiment namespace.

Remaining V4 blockers: historical build/tokenize/RSS gates, FTS13/15, lack of official CI, and unproven source of measurement/peak-memory variability. No final server switch or push was performed.
