# Four-variant paired performance campaign

User request: measure baseline, CRC-only, response-buffer-only and combined locally, screen regressions, then sync the same sources to Legion and test there.

Frozen production inputs: baseline a285f2e, combined 9db50ce. Single changes replace only common.h or main.cpp. No product changes are planned. Use the same compiler/options and explicit shared Mac compatibility transform locally; build original source on Linux.

- [ ] Build and hash all four variants; run correctness gates on each single variant.
- [ ] Prepare serial randomized ABBA/BAAB blocks, interleaving comparisons and same-binary A/A. Default workload: 10k×128, batch100, 1000 mixed TopK. Controls: batch1, batch10000, dimension8, short responses, above-64KiB responses followed by short responses. Keep response/WAL digests and launch/recovery, write/query wall times, process CPU/RSS.
- [ ] Smoke-test driver, deliberate response mismatch and invalid/timeout inputs, then freeze it before timings. Use block-level statistics and retain every sample. Small effects within A/A spread are inconclusive; engineering regression screen is 3%, assessed with block interval and noise, not a guarantee.
- [ ] Finish local analysis before uploading/testing on Legion. Resolve existing SSH/Tailscale connection, upload to a new dedicated directory, verify source hashes, build all four, run Linux correctness (including memory limits), then paired performance with original durability and SIMD code.
- [ ] Independently review measurements and report each optimization separately, limits and unresolved regressions. Save source snapshots, logs, hashes, commands and final outcomes locally.

No performance jobs may run in parallel on the same host. Remote connection/environment probes are read-only until local measurements finish. Deployment means sync to the user's test machine; it does not authorize publishing a GitHub PR or merging the team branch.
