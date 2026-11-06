# IK classify-once Implementation Plan

> Execute inline using superpowers:executing-plans; do not bundle H2.

**Goal:** Remove repeated UTF8MB4 decoding within one character classification, preserving classifications and errors.
**Architecture:** Decode once in the existing UTF8MB4 template specialization path, then use existing Unicode predicates in their original order. Leave UTF16/UTF16LE unchanged.
**Tech Stack:** C++20, existing macOS release toolchain; Linux performance acceptance remains required.
**Spec:** /Users/xiazihan/seekdb-work/results/FTS_IDEATION_REVIEW_2026-11-06.md H1.

## Constraints and review focus

Only change the existing IK header; no new cpp/h, no benchmark/golden edits. Separate branch feat/fts-ik-classify-once. Preserve classification precedence, malformed-input errors, output lifetime, and UTF16 behavior. Preserve clean base binary and data directories.

## Task 1: prove mechanism and equivalence

- [x] Inspect optimized baseline: UTF8MB4 classifier contains four decode call sites, so compiler has not removed them.
- [x] Capture baseline classifier output across Unicode/invalid input and baseline SQL output before editing.
- [x] Change only UTF8MB4 do_classify to decode once and retain the existing predicates and order.
- [x] Build, compare classifier/SQL outputs, verify decode call sites decrease to one, run FTS regression.
- [x] Record measured results and limitations; retain as a candidate only, without claiming Linux performance acceptance.

## Execution decisions

- User explicitly approved execution of the reviewed hypotheses, so proceed without another plan approval round.
- Existing seekdb-clean is already a linked worktree; reuse it on a new isolated feature branch.
- This is a performance-preserving refactor: baseline equivalence captures and the failing one-decode assembly criterion precede implementation; do not manufacture a failing semantic test for unchanged behavior.
- No automatic push or platform submission. No shared branch integration.

## Outcome
Rejected and rolled back after macOS three-pair full benchmark: tokenize_ik paired median improvement -1.97%, build_ik_all -8.61%. Mechanism 4→1 calls; 5,756,801 classification records identical; both FTS suites 14/15 with the same EXPLAIN-only failure. No production source diff remains. See handoff/fts-tuning/IK_CLASSIFY_ONCE_NEGATIVE_2026-11-06.md.
