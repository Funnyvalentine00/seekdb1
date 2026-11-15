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
Remote case: /home/xiazihan/benchmark/minivectordb-four-way-20261115-9db50ce via ssh legion. Uploaded archive SHA 0deec5d3d27c9aa7f973f0017d3c663ba0b56a00c58ae9091eddbb275580c35d; 304 file hashes verified. 4×22 Linux correctness checks passed (no skips).

Remote measurement ruling: batch1 10000 rows took ~21s per run, so operator interrupted the initial benchmark (exit 130; original logs retained). Only the Linux sync-heavy control was resized to `batch1-1k` (1000 rows, same batch=1 and durability); main workload and remaining controls unchanged. Final first-round output is `linux-paired-1k-control`, with `run-v2.log` and `exit-v2.code=0`. No candidate-dependent sample selection.

First remote complete round finished: exit-v2=0, 672 records archived and independently reviewed. Two noisy query controls remain unresolved, so diagnostic repeat was prepared (12+12 blocks, 10×query phase, CPU timing). Legion then became tailnet-offline and SSH/scp failed; no confirmatory remote run started. Ready local bundle handoff/mvd-four-way-20261115/confirm-bundle.tar.gz. User asked asynchronously to restore Tailscale/WSL.

User restored Legion. Original four binary hashes reverified; final confirmation archive verified and serial run started once (run-confirm.pid=871), then completed with exit-confirm.code=0. Responses and batch10000 each12+12 balanced blocks with queries×10, 192 diagnostic runs plus8 smoke runs. All200 phase CPU measurements complete, response/WAL digests match,400 stderr files empty; data directory empty. No original pilot samples pooled. Separate result archive downloaded and SHA verified: ada0a13fff250aeaa044f3b5e47d548533d0bf0ea0f45774b59861f72d04631d.

Confirmation outcome: responses/combined query −3.36% (95% interval −8.32% to +3.01%); initial slowdown not reproduced, but ±3% equivalence not established. Batch10000/CRC query +2.16% (+0.03% to +3.83%),9/12 blocks slower; query CPU +1.49% (−1.41% to +4.30%). Small query-regression risk remains unresolved. Read-only source/object audit found no direct query CRC work, but hot function addresses shifted by−0x50 after linking; no causal attribution or speculative product change. All requested local and Legion campaigns are complete; formal evaluation remains outside this campaign.

User follow-up: Legion is currently unplugged; user authorized pushing the current branch despite uncertain timing results. Per-run AC status and Windows power policy were not recorded, so keep Legion timings as diagnostic evidence only; do not infer mains-powered performance or attribute the slowdown solely to battery operation. Push `codex/minivectordb-round2-next` to origin without merging the competition branch or discarding recorded results.
