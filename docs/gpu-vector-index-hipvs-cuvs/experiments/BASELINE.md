# Reference versions and measurement rules

| Reference | Selection | Evidence status |
| --- | --- | --- |
| ORIGINAL_BASELINE | Historical pre-tuning CAGRA reference `e0bba2c32` is under review; earliest GPU proposal is `f796ef753` | Not yet proven to be the contest's exact baseline. Do not label reconstructed numbers official. |
| BEST_VALIDATED (bridge scope only) | E02 fused epilogue; code SHA in final report | Two5-pair groups:18.27%/18.11% lower build latency than f97; final GPU/lifecycle acceptance PASS. SQL/contest and1M unqualified. |
| PRIMARY_WORKING_REFERENCE | `f97bc2823` | CPU8/8, GPU helper and patched-dependency persistence PASS; preserved reference .so. |
| EXPERIMENTAL_CANDIDATE | None after E02 acceptance | No third hypothesis; incomplete1M group preserved. |

All comparisons must pin source commit, patch hash, dependency commits/patches,
bridge and observer hashes, compiler flags, detected GPU/BDF, dataset and ground
truth hashes, random seed, parameters, execution path and cold/warm state.

Use warmup followed by alternating A/B and B/A runs with at least five paired
samples before interpreting a performance delta. Keep every raw measurement.
Report medians, dispersion, paired ratios, Recall@10/@100, latency percentiles,
QPS, peak/persistent VRAM and host resources. Reject comparisons with differing
datasets, truth, parameters or dependency builds unless that difference is the
explicit experiment. A microbenchmark cannot establish SQL improvement.

Start with existing deterministic smoke fixtures and recover any existing contest
data before adding larger fixed-seed fixtures. Keep 128D and 256D separate. Measure
single query and batches independently. Use the current reference, rather than
only an old pre-optimization commit, to decide whether to retain a change.

An isolated AMD observer and full SQL smoke/restart checks now pass. Comparative
DDL/main-table-scan/SQL-QPS metrics remain UNMEASURED; bridge qualification does
not establish end-to-end gains. SMI reports100% utilization even between short
checks, at low reported board power; exclusive GPU idleness is not established.
