# Optimization inventory

Final current qualification supersedes the takeover snapshot table below:

| Item | Final state | Evidence |
| --- | --- | --- |
| E01 current GEMM versus BF | VALIDATED,250K bridge scope | Two5-pair groups,54.35-55.03% lower build latency, equal Recall |
| E02 fused norm,6062f81e2 | VALIDATED,250K bridge scope; PARTIAL overall | Two5-pair groups,18.11-18.27%, equal Recall/peak;1M reference timeout, no SQL performance/contest result |
| FP16 dependency descriptor repair | VALIDATED compatibility | Original failed file loads; half/float lifecycle and SQL restart pass |
| Offline snapshot integration | VALIDATED functional checks | SQL smoke12/12, default persistence6/6, forced-fusion persistence7/7 |

The following rows preserve the initial audit classification. References to
"current"/"pending" inside them describe takeover, not final acceptance.

Status applies to evidence currently available, not whether a commit exists.
Historical measurements below come from commit messages; raw runs have not been
recovered. `H` means historical-only evidence, not a current-server result.

| Optimization / commit | Status | Bottleneck and implementation | Files | Evidence / GPU status | Recall | Build | Search | Memory | Remaining risk |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Offline GPU snapshot `e0bba2c32` | UNKNOWN | Build GPU graph at DDL serialization; save/reload CAGRA; lazy VSAG | adaptor, bridge, plugin, DDL smoke | Source reviewed; current FP16 dependency reload fails | H correctness claims | Avoid CPU-only DDL route | GPU on restart | Holds graph and fallback vectors | Actual observer smoke pending |
| Degree/rounds tuning `a3944a43e` | UNKNOWN | igd 256→128, NN rounds 100→60; reusable search scratch | bridge C | H paired timings only | H unchanged | H 128D 43→21 s; 256D 161→22 s | H fewer allocations | Reused scratch | New server/data validation needed |
| Exact kNN graph `c35c46d2c` | UNKNOWN | BF graph then IndexFromArgs, cheaper search | bridge C, adaptor | H GPU data only | H SIFT @10 .9977 / @100 .9997 | H 21→5.8 s GPU | H 5.7→3.3 ms | BF scratch | Norms/precision/ties |
| Per-index buffers + FP16 `d3358d41d` | UNKNOWN | Reuse buffers across rotating SQL workers; half input | bridge C | Current small FP32 lifecycle PASS; full original dataset pending | H unchanged for SIFT integers | H 5.7→3.5 s GPU | H 1.51–1.68x QPS | Persistent scratch | Noninteger FP16 and concurrency |
| gd64 / chunked BF `58162cf09` | UNKNOWN | Reduce selection workspace, bounded queries; retain backing data | bridge C | H platform VRAM failure and corrected run | H @10 .9983 | H 12.5 s E2E | H 1.63x QPS | H 922→354 MB | Historic configuration superseded |
| Sharded BF `1f59472e6` | UNKNOWN | Four shards and host top-k merge; restore gd128 | bridge C | H results; current small fallback lifecycle PASS | H @10 .9998 | H GPU 4.96 s | H 1.48x QPS | H 522 MB | Host copies/merge; short shards |
| Two shards/gd96 `61a845c1c` | UNKNOWN | Lower candidate work; 256D NN rounds52 | bridge C | H results only | H SIFT .9993 | H SIFT E2E 7.9 s | H combined query1.63x | H 632 MB | Later returned to four shards |
| 256D AUTO `0a2fb0cb9`, `397e4c428` | UNKNOWN | AUTO instead of NN descent; degree160 | bridge C | H results only | H .9335 | H E2E25.2→16.4 s | H QPS218.6 | H within gate | Verify current dependencies |
| DDL row-file memory `c94bafb33` | UNKNOWN | Memory threshold512KB→8MB | DDL row-file path | H row I/O54 ms | No intended change | H scan dominates remaining2.9 s | No intended change | More host memory | Need current SQL profile |
| NUMA pin/copy/quarantine `81a7075bc` | UNKNOWN | GPU-node first touch, temporary thread affinity | bridge C | Actual node0/PCIe verified; benefit not rerun | H unchanged | H engine21.5→17.4 s | Not measured | Extra host copy | Global affinity/watch state; concurrency |
| 256D IVF_PQ `dd3f063cf` | FAILED | Faster graph alternative | bridge C | H rejected by relative VRAM gate; default reverted | H .975 | H2x build | H +2% search cost | H2.8–4GB | Keep optional; no default promotion |
| Wide search pool `7bcc8810d` | UNKNOWN | 256D itopk floor1024 | bridge C | H results only | H .982 | AUTO restored | H +2% cost | Scratch grows | Must measure Recall@100 |
| Graph degree96/128 `f9d4ae22d` | UNKNOWN | 128D BF96, 256D AUTO128 | bridge C | H results only | H1.0 / .973 | H8.49 /15.52 s E2E | Not rerun | H614 MB | Actual default BF is not symmetrized unless algo contains `sym` |
| WMMA/GEMM `92dff65cf`, fallback `f6a8e1dc3` | EXPERIMENTAL | Tensor-core distances + cuVS top-k; fail-soft BF | GEMM, bridge C, build | Current real kernels/full helper PASS; current full snapshot qualification incomplete | Current small exact-order checks PASS | H3.75x vs old baseline | No current throughput evidence | H570–576MB | Expanded-L2 error; streams; optional architecture |
| Norms/top-k/cleanup `f97bc2823` | EXPERIMENTAL | Correct base offset/query norms; gd+32→gd; short tiles; owned BLAS handles | GEMM, build, CPU tests | CPU8/8; GPU10/10 combinations PASS | Small CPU-oracle Top-K PASS | 25% candidate-count reduction only | Not measured | Candidate arrays4→3MiB at2048x96 | No measured speedup or contest result |
| FP16 dependency reload repair | EXPERIMENTAL | Writer emits `e2`; C reader accepts only `f2` | Isolated hipVS cagra_c.cpp | FP16 FAIL reproduced; FP32 control PASS | Search itself PASS | No performance claim | No performance claim | No intended change | Must preserve snapshot compatibility and record patch |

Next promotion requires current-source GPU correctness, preserved Recall,
repeatable paired performance improvement and acceptable resource usage.
