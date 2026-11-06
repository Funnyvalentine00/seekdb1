# Experiment budget and early stopping

User policy received 2026-11-07; it overrides the earlier open-ended mission.

- Maximum independent GPU hypotheses: **6**.
- Implementation corrections per experiment: **2** maximum.
- Formal paired GPU measurement groups per valid candidate: **3** maximum.
- Primary promotion: **≥3%**, above observed noise; preserve original Recall@10/@100 gates and acceptable memory/stability; prove optimized path ran.
- After two consecutive no-gain experiments: reassess direction. After three: stop exploration.
- Also stop on untrustworthy/unstable GPU measurements, insufficient expected value, unsafe preservation, deadline or exhausted budget.
- One large GPU benchmark at a time. Preserve raw results and record temperature/utilization/memory. Never delete user assets to free space.

## Execution timebox (agent scheduling choice, not a user-specified duration)

Task takeover: **2026-11-07 00:29:59 Asia/Shanghai** (goal creation timestamp).
Total window: **120 minutes**. New exploration stops by **01:59:59**.
Final acceptance/delivery reserve: **30 minutes (25%)**.
Delivery deadline: **02:29:59**. Finish earlier if appropriate. Do not start an
experiment whose validation cannot fit before the acceptance boundary.

## Counters

Independent GPU optimization experiments started/completed: **2 / 2**, out of **6**.
Success / partial / no-gain or failed: **1 / 1 / 0**. E01 qualifies existing GEMM;
E02 passes the250K GPU promotion gate, but1M/SQL performance remains unqualified.
Consecutive no-gain experiments: **0**.
Formal paired groups attempted/completed: **5 / 4**. E01 **2/3**, E02 **3/3**:
two valid five-pair groups each; E02 group3 stops at a240s reference timeout,
with zero complete pairs and no candidate run. No fourth E02 group.
Implementation corrections: E01 **0/2**, E02 **2/2**, conservatively counting
two validation-tool repair batches (active-path/device checks; SONAME isolation).
No GPU kernel correction or parameter search was needed.
The final native SQL adapter needed a console-versus-application-log oracle fix;
this is acceptance-tool adaptation, not another performance candidate revision.
All formal E02 samples and its production commit remained unchanged.

Exploration stopped early at **01:39:55 Asia/Shanghai**, leaving about50 minutes
of the120-minute window for acceptance/delivery (more than25%). Stop reasons:
the1M reference stall makes scale-up measurements untrustworthy; the measured
winner is preserved and further mechanisms have lower value than qualification.
No new hypotheses or large exploratory GPU runs after this point. Final unpaired
acceptance runs recheck correctness/resources/QPS, not paired promotion groups.

Environment installation, compiler/runtime repairs and correctness qualification
so far are admission work, not independent performance hypotheses. E01 will
conservatively count the existing GEMM-versus-fallback performance hypothesis as
one experiment. Parameters within a mechanism do not create extra slots or
permit an unbounded search. Record every implementation correction explicitly.

No large GPU benchmark is authorized until its CPU correctness, compilation,
HIP status, VRAM lifetime and relevant boundary checks pass. Actual measured
results belong in `EXPERIMENT_LOG.md`, including no-gain candidates and early
stop decisions. Delivery ends the task; no automatic next round.
