# MiniVectorDB parallel optimization experiments

Exploration baseline: `fe92d5bb2f8f7727354ff4a4914c9a106fa18d8b`.
The product source remains at that baseline on this experiment branch. Patches
are applied to isolated copies by `validate_variants.py`.

The three independent candidates are WAL frame allocation (`storage.cpp`),
fused equality-filtered TopK (`engine.cpp`), and one-time AVX2 scan dispatch
(`vecops.cpp`). The matrix includes baseline, each candidate, each pair, and
all three together. Every combination must pass correctness checks before
being added to `binaries.json` for timing.

`bench_matrix.py` uses the actual JSON Lines protocol and a fixed 10k-row,
128-dimension dataset. Each run inserts batches of 100 and executes 1000 exact
TopK queries with K=10; half use an equality filter across 100 categories.
Each candidate is paired with a fresh baseline run five times, alternating
order. Server and client are pinned to separate CPUs when available. All
timed query outputs must have the same digest as baseline. Reported metrics
are write/query elapsed time, filtered/unfiltered portions, and process peak
RSS. No platform score is inferred from these local measurements.

The exploration workflow builds and tests all variants on one Linux runner,
then times them serially and uploads raw logs and JSON results. Its branch
trigger is limited to `perf/minivectordb-round2-explore-20261112`. The official
checker is pinned to upstream `f4d4e26653af823a7601668802f80e374ad6643c`.
