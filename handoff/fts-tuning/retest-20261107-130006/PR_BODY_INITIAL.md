This PR submits the existing integrated v4 FTS candidate for verification. Product commit: `194150497aca9a69e395317bd6d5a64f26340a61`. It preserves the v3 sparse retrieval and default TOKENIZE fast paths, adds H2 batch projection, H3 scoreless MATCH handling, H7 raw array access, and the subsequent MATCH boundary fixes.

The boundary fixes keep score-dependent lookup/index-merge paths intact and handle long scoreless queries beyond 256 terms. No V5 experimental product change is included.

Validation on the sealed Linux reference:
- FTS: 13/15; known failures are `basic_dml` and `partitioned_simple_query`. File SQL: 1/1. AI column: 5/5.
- Nine MATCH boundary groups, TOKENIZE/output/error differentials, six-decimal score comparisons, prepared statements and 80 concurrent requests matched the recorded reference.
- Historical v3-to-v4 measurements show query gains, but build/tokenize/RSS gates remain unresolved. Recent same-version calibration was noisy; a power-controlled legion retest is pending. This is not a fully accepted BEST_VALIDATED version.

[Detailed Linux acceptance evidence](https://github.com/Funnyvalentine00/seekdb1/blob/a00ff8c1d724cfe12f0d5b7e9f536c5490104d5c/handoff/fts-tuning/LINUX_V4_ACCEPTANCE_2026-11-06.md) and [latest same-version calibration](https://github.com/Funnyvalentine00/seekdb1/blob/a00ff8c1d724cfe12f0d5b7e9f536c5490104d5c/handoff/fts-tuning/next-cycle-20261107-104032/DELIVERY.md).

Verification only: keep FTS on its independent branch; do not merge into `feat/seekdb-all`. Requesting benchmark/CI results, without changing official tests, golden outputs, data or scoring. Refs #6.
