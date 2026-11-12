# Filtered TopK and WAL allocation measurements

Baseline: `fe92d5bb2f8f7727354ff4a4914c9a106fa18d8b` on
`Funnyvalentine00/seekdb1:2026-competition-round2`.

Keep two independent changes: fuse non-primary-key BIGINT/VARCHAR equality
filtering with ascending-distance TopK, and serialize each WAL frame into one
pre-sized allocation. The first avoids collecting every matching row before
ranking; the second removes repeated copying of the growing WAL payload.
The separate unfiltered SIMD scan prototype did not demonstrate a useful gain
and is excluded.

## Method and evidence

- [First experiment](https://github.com/Funnyvalentine00/seekdb1/actions/runs/43162975046),
  harness commit `b74bcb877ad3d66891e701b1b2e97f3e3c6c5f8c`, AMD EPYC 9V74.
- [Second experiment](https://github.com/Funnyvalentine00/seekdb1/actions/runs/43164106610),
  harness commit `3f867ca58136b7e0d75b7b0327d5e0a8e1385bbe`, AMD EPYC 7763.
- Both use Ubuntu 22.04, the same compiler/options for baseline and candidates,
  a server pinned to CPU 0 and client pinned to CPU 3. Candidates run serially.
- Five fresh-database baseline/candidate pairs per comparison, alternating
  execution order. Percentages are the median of per-pair candidate/baseline
  changes; a negative change means less time or memory. Absolute times from
  different runners are not compared.
- JSON Lines workload: 10,000 rows, 128 dimensions, K=10, exact mode, 1,000
  queries split equally between unfiltered and equality-filtered queries.
  Each equality filter matches approximately 1% of rows. Default insert batch
  is 100. The additional batch-size runs use 100 queries.
- Requests are prepared before timing. Write time includes parsing, WAL writes
  and syncs. Response validation/hashing is outside the timed loops. Every
  timed query result must match the baseline digest. All 160 timed runs passed.
- Second-run process CPU time comes from Linux `/proc/PID/schedstat`; peak RSS
  comes from `/proc/PID/status`. Raw logs, summaries, machine metadata and
  source/binary hashes are in each run's artifacts (14-day retention).
- Reproduction harness, candidate patches and exact-byte checks are retained
  on the experiment branch at the revisions above; run
  `experiments/minivectordb/validate_variants.py`, then `bench_matrix.py` as in
  `.github/workflows/minivectordb-explore.yml` at those revisions.

## Query results (default batch 100)

| Candidate | First run mixed-query change | Second run mixed-query change | Second run filtered-query change |
|---|---:|---:|---:|
| Filtered TopK | -14.92% | -16.97% | -35.06% |
| WAL + filtered TopK | -17.24% | -18.32% | -37.77% |
| Unfiltered SIMD scan prototype | -0.78% | Not promoted | Not applicable |

For the combined candidate on the second runner, median mixed-query elapsed
time was 189.055 ms baseline versus 154.371 ms candidate. Its unfiltered portion
changed -0.29%, and query CPU time changed -20.01%. These are measured combined
results, not sums of standalone improvements.

## Write results (second run)

| Rows per INSERT | WAL-only elapsed change | WAL-only CPU change | Combined elapsed change |
|---:|---:|---:|---:|
| 1 | -0.05% | +0.24% | -2.97% |
| 100 | -1.94% | -2.86% | -3.20% |
| 1,000 | -37.33% | -37.82% | -37.06% |
| 10,000 | -87.86% | -87.89% | -87.94% |

WAL-only median elapsed time for batch 1,000 was 574.870 ms versus 359.655 ms;
for batch 10,000 it was 3366.436 ms versus 408.647 ms. Peak process RSS in the
batch-10,000 comparison fell 15.43%. Single-row writes are effectively flat.
First-run write times were variable (including a WAL+scan regression), so the
decision to keep WAL relies on the batch/CPU measurements rather than the
first-run write averages. WAL alone had roughly 1–3% higher query elapsed time
in the second run; the combined candidate retained the filtering gain.

## Correctness and limits

Both experiments passed all eight combinations of the three prototypes and
baseline. Each ran the pinned official checker and 20 public cases, 13 SQL
regressions, JSON Lines checks, query boundaries, TopK memory checks, seed-1
60-round fuzzing and the focused filtered-query oracle. Filtered combinations
also passed a 150,000-row test with only 1 MiB extra virtual-memory allowance.
The WAL byte/CRC/framing comparison passed 100 cases; the scan comparison
passed 2,460 exact-bit/oracle cases, without establishing a useful scan speedup.

The production filtered regression uses 257 rows to exercise heap growth beyond
the initial 128-entry reserve. Transactions with pending writes, primary-key
lookups and unsupported predicate/order shapes keep their existing paths.
WAL field bytes, CRC coverage, sync ordering and recovery code are unchanged.

These are synthetic same-runner comparisons, not a new competition submission
or a prediction of platform E. They do not establish improvements for hidden
workloads, all selectivities, all vector dimensions or every commit size.
