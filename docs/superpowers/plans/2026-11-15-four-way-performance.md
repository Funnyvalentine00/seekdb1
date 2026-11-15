# Four-variant paired performance campaign

User request: measure baseline, CRC-only, response-buffer-only and combined locally, screen regressions, then sync the same sources to Legion and test there.

Frozen production inputs: baseline a285f2e, combined 9db50ce. Single changes replace only common.h or main.cpp. No product changes are planned. Use the same compiler/options and explicit shared Mac compatibility transform locally; build original source on Linux.

- [x] Build and hash all four variants; run correctness gates on each single variant.
- [x] Prepare serial randomized ABBA/BAAB blocks, interleaving comparisons and same-binary A/A. Default workload: 10k×128, batch100, 1000 mixed TopK. Controls: batch1, batch10000, dimension8, short responses, above-64KiB responses followed by short responses. Keep response/WAL digests and launch/recovery, write/query wall times, process CPU/RSS.
- [x] Smoke-test driver, deliberate response mismatch and invalid/timeout inputs, then freeze it before timings. Use block-level statistics and retain every sample. Small effects within A/A spread are inconclusive; engineering regression screen is 3%, assessed with block interval and noise, not a guarantee.
- [x] Finish local analysis before uploading/testing on Legion. Resolve existing SSH/Tailscale connection, upload to a new dedicated directory, verify source hashes, build all four, run Linux correctness (including memory limits), then paired performance with original durability and SIMD code.
- [x] Independently review measurements and report each optimization separately, limits and unresolved regressions. Save source snapshots, logs, hashes, commands and final outcomes locally.

No performance jobs may run in parallel on the same host. Remote connection/environment probes are read-only until local measurements finish. Deployment means sync to the user's test machine; it does not authorize publishing a GitHub PR or merging the team branch.

Local campaign finished: 672 runs, no repeatable regression signal, CRC default warm reopen about 50% faster; buffer query effect inconclusive. See handoff/mvd-four-way-20261115/macos-results.md.
Remote case: /home/xiazihan/benchmark/minivectordb-four-way-20261115-9db50ce via ssh legion. Uploaded archive SHA 0deec5d3d27c9aa7f973f0017d3c663ba0b56a00c58ae9091eddbb275580c35d; 304 file hashes verified. 4×22 Linux correctness checks passed (no skips); paired timing is running serially, monitor run.log and exit.code, do not restart while run.pid is active.

Remote measurement ruling: batch1 10000 rows took ~21s per run, so operator interrupted the initial benchmark (exit 130; original logs retained). Only the Linux sync-heavy control was resized to `batch1-1k` (1000 rows, same batch=1 and durability); main workload and remaining controls unchanged. New output `linux-paired-1k-control`, monitor `run-v2.log` and `exit-v2.code`; PID file `run-v2.pid`. No candidate-dependent sample selection.

First remote complete round finished: exit-v2=0, 672 records archived and independently reviewed. Two noisy query controls remain unresolved, so diagnostic repeat was prepared (12+12 blocks, 10×query phase, CPU timing). Legion then became tailnet-offline and SSH/scp failed; no confirmatory remote run started. Ready local bundle handoff/mvd-four-way-20261115/confirm-bundle.tar.gz. User asked asynchronously to restore Tailscale/WSL.
