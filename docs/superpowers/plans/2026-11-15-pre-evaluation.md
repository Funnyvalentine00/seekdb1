# MiniVectorDB pre-evaluation implementation plan

**Goal:** Prepare independently reviewable optimization candidates and a reproducible Linux evaluation entry point.
**Architecture:** Preserve the engine and disk/protocol contracts; optimize CRC, response allocation and per-commit mapping growth in place.
**Tech Stack:** C++17, POSIX, Python standard library.
**Spec:** ../specs/2026-11-15-pre-evaluation-design.md

## Constraints and review focus

No change to WAL bytes, IEEE polynomial, commit durability ordering, SQL results or vector algorithm. Inspect nonaligned/tail CRC reads, seeded chaining, large-to-small output reuse, error response isolation, and growth/recovery behavior. No claimed Linux performance until measured there.

- [x] Freeze baseline, preserve current remote documentation, capture source/binary hashes.
- [x] CRC: independent reference test first, then slicing-by-8 in src/common.h; test known vectors, lengths, offsets, seeds and chained calls with sanitizers.
- [x] Response buffer: add protocol transition regression, implement bounded reuse in src/main.cpp; verify JSONL and local SQL formatting.
- [x] Storage decision: defer preallocation until Linux fault injection can verify mapping failure/restart. Add full-binary WAL byte compatibility and cross-version recovery tests for CRC.
- [x] Evaluation: prioritize the user-requested local correctness campaign; add tests/validate.py and Linux instructions. New performance runner deferred until target access; existing benchmark driver remains available. Validate protocol mismatch and sanitizer-failure detection with deliberate negative controls.
- [x] Run available full regression suites against temporary Mac compatibility build, record limitations, obtain independent whole-diff review, fix material findings, record final evidence.

Execution decision: continue within the user's authorized optimization scope; use focused parallel implementation for disjoint files and one final independent review. Performance refactors retain semantics, so correctness characterization tests are expected to pass the baseline; allocation/growth diagnostics establish changed work without timing assertions.

Final ruling: preallocation remains deferred; no source changes beyond CRC and stdio response allocation. All available final correctness gates passed; two Linux-only memory checks are explicitly skipped on Mac. No push, merge or official performance claim.
