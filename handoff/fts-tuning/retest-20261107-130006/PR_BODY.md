This PR submits the existing integrated v4 FTS candidate for verification. Product commit: `194150497aca9a69e395317bd6d5a64f26340a61`.

It preserves the v3 sparse retrieval and default TOKENIZE fast paths, integrates H2 batch projection, H3 scoreless MATCH handling, H7 raw array access, and the subsequent MATCH boundary fixes. Scored lookup/index-merge paths remain intact, and long scoreless queries beyond 256 terms are covered. No new V5 experimental product change is included.

Historical local Linux evidence against v3 (`7accd1ace`), six paired runs, observed median query time reductions of **30.41% CN, 30.57% Beng, 28.91% mixed, and 5.20% limit**. These provide the basis for submitting the existing candidate for platform testing. The same historical measurements also show IK content build 1.50% slower, Beng tokenize 2.14% slower, and peak RSS 2.67% higher; those gates remain unresolved. These are historical observations, not new V5 gains or platform scores.

Fresh Linux verification on the sealed reference:
- FTS 13/15: `basic_dml` and `partitioned_simple_query` still fail. File SQL 1/1; AI column 5/5. Total 19 passed, 2 failed.
- Nine MATCH boundary groups and projection/score-controls/TOKENIZE output/error differentials match the recorded reference.
- After the user confirmed maximum performance settings and an idle host, seven same-version official reports were retained: three complete A/A pairs and one unpaired report. Every report has hits 8001/11000/7332/20; 49,000 latency samples are preserved.
- Variability remains above the predeclared gates: IK pair changes -6.81/+7.16/-3.08%; Beng -0.70/+6.93/-4.48%; SELECT1 +20.04/+8.86/+15.26%. These are same-version timing differences, not product gains. No new optimization was promoted.

[Full report, raw evidence and offline reproduction](https://github.com/Funnyvalentine00/seekdb1/blob/feat/fts-opt-v5/handoff/fts-tuning/retest-20261107-130006/DELIVERY.md). Historical v3-to-v4 query gains and unresolved build/tokenize/RSS gates remain documented separately. This is not a fully accepted BEST_VALIDATED version.

`/benchmark` was requested; no check/Actions run or bot result has been observed. The results above are actual legion Linux runs, not platform CI. Official scripts, golden outputs, data and scoring are unchanged.

Verification only: keep FTS on its independent branch; do not merge into `feat/seekdb-all`. Refs #6.
