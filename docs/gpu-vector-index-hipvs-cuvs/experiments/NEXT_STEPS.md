# Deferred queue after bounded delivery

Do not automatically execute this queue. This task ends after acceptance/delivery.

1. Diagnose the f97 reference1M timeout with a bounded reproducer and API stack
   evidence before scaling. The profile stall and1M timeout have no proven
   common root cause. Preserve both logs.
2. Repeat the passing SQL smoke/restart checks in the official evaluation build,
   with matched patched hipVS runtime. Establish official baseline/Recall gates;
   deterministic research fixtures do not qualify contest performance.
3. If new profiling supports it, investigate Top-K/merge. Partial E01 profile:
   selection36.40%, merge10.70%. Preserve sorting, ties and cross-tile offsets.
4. Assess graph build, NUMA/transfers and search/allocation reuse only under a
   newly authorized budget. Do not mechanically consume all six slots.

Static review findings needing reproduction before changes: global registry lock
and per-query pthread cost; global bridge affinity/VRAM-watch state under concurrent
builds; ignored output-distance copy error; duplicated snapshot vector data;
default `bfknn` does not enable symmetrization despite some historical comments.
These are hypotheses/risks, not verified performance bugs.

At two no-gain experiments reassess. At three, or untrustworthy GPU conditions,
stop new exploration. Enter final acceptance by the timestamp in `BUDGET.md`,
produce the bounded final report and stop after delivery.
