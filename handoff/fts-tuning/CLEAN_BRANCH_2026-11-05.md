# FTS clean branch: code scope, evidence, and regression status

Date: 2026-11-05
Branch: `feat/fts-clean-v5`
Base: `5bce9e06d1281347ad61344e4b01fab9158c58a2` (`upload 2026-competition testcases`)
Code commit: `6b29a178d55e8524ada8a6d8cab2c4dc9cdb410d`
Code diff SHA256: `b74aadde9e490ae0fc118cd00af6492529a76ea82e03ec5ec9b789d8a7ae7369`
([`CODE_DIFF_6b29a178.patch`](CODE_DIFF_6b29a178.patch))

## What is in this branch

Six existing files, `+70 / -6`:

| File | Change |
|---|---|
| `src/storage/retrieval/ob_sparse_daat_iter.{h,cpp}` | `ObSRMergeCmp` compares binary doc ids with `memcmp + length`, fallback to `null_first_cmp_` for null/non-string ids |
| `src/storage/retrieval/ob_text_retrieval_token_iter.{h,cpp}` | `ObTextRetrievalDaaTTokenIter::advance_to` uses the same binary comparison fast path |
| `src/sql/engine/expr/ob_expr_tokenize.cpp` | skip `reform_parser_properties` when `arg_cnt <= 2`; `try_load_dictionary_for_ik` still runs |
| `src/storage/fts/ob_fts_parser_helper.cpp` | initialize the same default parser properties directly when `parser_properties` is empty |

Removed from the earlier v4 candidate, at the captain's request:

- `ObSRDaaTIterImpl::fill_merge_heap` raw `get_data()` array pointers.
- `ObSRBMWIterImpl::get_iter` raw array access; the file is restored to base.

These two hunks had small positive local readings in their original single-point
reports, but were not retained in the clean submission. The retained set is the
two binary comparator fast paths plus the tokenize default-properties fast path.

No BMW file, File SQL file, build file, benchmark script, or golden result is changed. No `.cpp`/`.h` is added or deleted.

## Performance evidence

The final same-machine Linux x86-64 A/B for the full v4 patch is in
[`FINAL_CUMULATIVE_2026-11-04.md`](FINAL_CUMULATIVE_2026-11-04.md): query
`+2.59% ~ +8.88%`, `tokenize_ik +4.15%`, build flat, all hits exact, no
official metric regressed by more than 1%.

That run included the two `get_data()` hunks later removed from this branch, so
it is an upper-bound reference for the clean set. The retained comparator
reports are [`QOPT_BINCMP_RESULTS.md`](QOPT_BINCMP_RESULTS.md) and
[`QOPT_DAATCMP_RESULTS.md`](QOPT_DAATCMP_RESULTS.md); their query gains are the
direct evidence for the two comparator changes.

Tokenize semantics were checked on 21 statements (ik/beng/space/ngram/ngram2,
empty string, NULL, emoji, >4KB text, custom properties): byte-identical output,
`checked=21 mismatches=0`
([`TOKENIZE_FASTPATH_2026-11-04.md`](TOKENIZE_FASTPATH_2026-11-04.md)).

The MATCH path was checked separately: benchmark FULLTEXT indexes carry
non-empty `parser_properties`, so MATCH does not take the empty-property fast
path. See
[`MATCH_PATH_ANALYSIS_2026-11-05.md`](MATCH_PATH_ANALYSIS_2026-11-05.md).

## Exact clean binary and regression

Build: `./build.sh release --make -j10` on macOS arm64 from this branch.

- Binary: `build_release/src/observer/seekdb`
- SHA256: `644a06e593120eeda2c81a8ad58f8dc834abaa027eaa58637d46e4610130868c`
- Startup source revision: `1-6b29a178d55e8524ada8a6d8cab2c4dc9cdb410d` (no `-dirty`)

`fts_index`: **13/15 for both the clean candidate and the baseline**, with the
same two pre-existing failures.

| Case | Clean candidate | Baseline | Notes |
|---|---|---|---|
| `basic_dml` | FAIL | FAIL | test variable `$rowkey_doc_table_id` unresolved: `... table_id = No such row`; diffs identical except timestamp |
| `partitioned_simple_query` | FAIL | FAIL | only EXPLAIN bind label differs (`:0` vs `:1`); diffs identical except timestamp |
| other 13 cases | PASS | PASS | includes `tokenize_function`, `parser_properties`, `simple_query`, `cn_word`, DDL/index cases |

Evidence:

- Candidate summary: [`regress/fts_index-clean-candidate-2888.txt`](regress/fts_index-clean-candidate-2888.txt)
- Baseline summary: [`regress/fts_index-baseline-full-2889.txt`](regress/fts_index-baseline-full-2889.txt)
- Candidate diffs: [`regress/basic_dml.clean-candidate-2888.diff`](regress/basic_dml.clean-candidate-2888.diff), [`regress/partitioned_simple_query.clean-candidate-2888.diff`](regress/partitioned_simple_query.clean-candidate-2888.diff)
- Baseline diffs: [`regress/basic_dml.baseline-2889.diff`](regress/basic_dml.baseline-2889.diff), [`regress/partitioned_simple_query.baseline-2889.diff`](regress/partitioned_simple_query.baseline-2889.diff)
- Server revisions: [`regress/clean-candidate-server-2888.log`](regress/clean-candidate-server-2888.log), [`regress/baseline-server-2889.log`](regress/baseline-server-2889.log)

Both fresh instances had the test's `admin` account created before the suite;
without it, `cn_word` and `partitioned_simple_query` fail at their `connect`
statement, which is an environment failure rather than a product regression.

`ai_column`: 4/5; `ai_column_retry_strict` fails at line 108 on unfinished AI
tasks. The same case fails identically with the baseline binary on an isolated
data directory (`~/seekdb-work/data/ai-check`), as recorded in
[`QOPT_DAATCMP_RESULTS.md`](QOPT_DAATCMP_RESULTS.md). Raw candidate log:
[`regress/ai_column_retry_strict.candidate-2026-11-03.log`](regress/ai_column_retry_strict.candidate-2026-11-03.log).

`file_sql`: not runnable on macOS; the implementation returns
`OB_NOT_SUPPORTED` on the non-Linux branch and the official fixture requires a
Linux setup. Must be checked by the official Linux CI.

## `git_dirty`

This branch is self-contained: code and evidence are committed, and submission
must be made from this worktree only. The platform `git_dirty` flag should be
`0`; verify with:

```bash
git status --porcelain
```

An empty output means no dirty files. The build startup line already reports
`1-6b29a178...` without a `-dirty` suffix. The unrelated uncommitted File SQL
edits in the separate `~/seekdb-work/seekdb1` worktree are not part of this
branch and must not be submitted.

## Limitations

- The Linux x86-64 node used for A/B expired before this clean-branch assembly;
  the final cumulative numbers were measured with the full v4 patch. The two
  removed hunks had modest local single-point gains but were not retained; the
  exact clean branch has been built and regression-tested locally, but not
  re-benchmarked on Linux.
- Local absolute timings and the official container are not comparable; the
  official `/benchmark` result remains the source of truth.
- `file_sql` and the platform's exact `git_dirty` computation must be confirmed
  by CI.

