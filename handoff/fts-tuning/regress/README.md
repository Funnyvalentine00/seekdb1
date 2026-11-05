# Raw regression evidence

## Environment

- macOS arm64, release build.
- `mysqltest` from `~/devtools/seekdb-deps/3rd/u01/obclient/bin/mysqltest`.
- Each run used a fresh data directory and a dedicated port; the baseline and
  candidate never shared a data directory.
- `basic_dml` and `partitioned_simple_query` need the `admin` account used by
  their `connect` statements. The fresh instances used for the final baseline
  reproduction had `admin` created before running the suite.

## Candidate

- Exact clean branch binary: `build_release/src/observer/seekdb`, SHA256
  `644a06e593120eeda2c81a8ad58f8dc834abaa027eaa58637d46e4610130868c`.
- Startup source revision: `1-6b29a178d55e8524ada8a6d8cab2c4dc9cdb410d`.
- Result: `fts_index-clean-candidate-2888.txt` (`13/15`).
- `tokenize_function`, `parser_properties`, `simple_query`, and all DDL/index
  cases pass.

## Baseline

- Binary: `seekdb-baseline`, SHA256
  `ccb4c581bc1d58b380407e35e00942eb21c8e0fa229aafc0b6abe0edc92cbcbb`.
- Full suite with `admin` created (2026-11-05):
  `fts_index-baseline-full-2889.txt` (`13/15`).
- `basic_dml` and `partitioned_simple_query` fail on both binaries; the diffs
  are identical except for timestamps.
- An instance without the `admin` account fails `cn_word` and
  `partitioned_simple_query` at their `connect` statements; that is an
  environment failure, not a product regression.

## Known failures

- `basic_dml`: the test's `$rowkey_doc_table_id` is unresolved, so the emitted
  SQL contains `table_id = No such row`. The baseline fails at the same line.
- `partitioned_simple_query`: only the EXPLAIN bind label differs (`:0` vs
  `:1`). The baseline diff is identical.
- `ai_column_retry_strict`: fails at line 108 on unfinished AI tasks; the
  baseline binary reproduces it on an isolated data directory. Raw candidate
  log is included; the baseline server startup log shows the source revision.
- `file_sql`: not runnable on macOS; the implementation returns
  `OB_NOT_SUPPORTED` on the non-Linux branch.
