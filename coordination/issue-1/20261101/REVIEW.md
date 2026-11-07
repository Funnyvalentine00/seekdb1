# Coordination review: fixed snapshot dade8da47eacc6522f4ab165a251f49335e21775

Policy: .agents/skills/code-review/SKILL.md. This was a read-only contract and
handoff review, not a full review or runtime qualification of all new code.
Native HEAD and the working patch were stable during capture at18:00:10+08.
Files and logs may have changed afterward; findings refer to this snapshot.

## 1. Blocker: DESCRIBE drops the source authorization requirements

Location: src/sql/resolver/ddl/ob_explain_resolver.cpp:140, captured under
native-snapshot/committed/. After resolving a File SQL-containing source query,
the resolver returns a constant metadata SELECT via set_basic_stmt(metadata).
The normal check at src/sql/ob_sql.cpp:2270 therefore receives that constant
statement instead of the native tables referenced by the source query. The
unchanged privilege checker traverses the returned statement; normal EXPLAIN
instead retains and authorizes its child.

Concrete trigger: a user without SELECT privilege on secret_schema.secret,
but with an allowed CSV path, issues:

    DESC SELECT t.* FROM secret_schema.secret t
    CROSS JOIN read_csv('/allowed/basic.csv') f;

The source table's column metadata can reach the generated result without its
normal privilege requirement. Preserve source authorization through metadata
generation and prepared execution. Require a restricted-user regression that
rejects the mixed query and verifies the corresponding EXPLAIN behavior.
Evidence is the traced static call path; this chat did not run the repro against
the implementation owner's instance.

## 2. Major: native TINYINT projections are mislabeled BOOLEAN

Location: src/sql/resolver/ddl/ob_explain_resolver.cpp:53. The mapper returns
BOOLEAN for every ObTinyIntType. Native TINYINT columns use the same enum and
can contain2, so a mixed native-table/File SQL projection reports a false type.
Retain enough provenance to distinguish file booleans; a focused regression
should describe a native TINYINT value2 alongside an inferred file BOOLEAN.
This is a static finding; no runtime reproduction was claimed.

## 3. Major: the delivery commit omits required working-tree integration

Location: NATIVE_EVIDENCE.json working_source_files and native-snapshot/working.patch.
At capture, five source files were modified after dade8da: parser enums, static
code generation, file_sql_scan.cpp/.h and the table-mode definition. These are
functional COPY/Parquet/glob integration, not unrelated scratch files. Checking
out the stated commit alone does not reproduce the captured working source.
Commit the intended, validated integration and attach build/test identity to
that final commit. Do not attribute mixed-tree binaries to dade8da alone.

## 4. Major: raw verification evidence is incomplete

Locations: native-snapshot/handoff/file-sql/logs/build.log:1 and verify.log:1.
build.log contains commands and a claimed exit code, but only a short compiler
tail and explicitly says the full output was summarized. verify.log contains
result text and a trailing 'ok', without the full official runner command,
exit code, per-case runner status or source/binary identity. test.log similarly
contains illustrative commands/output rather than a fully attributable run.
Issue1 sections4/7 require complete logs. Preserve the actual final build and
official-run output, command, exit status and binary SHA256 before accepting
the delivery; the existing text alone does not establish a passed suite.

## 5. Major: the FTS handoff's current-version entry is stale

Location: Windows handoff/fts-tuning/README.md sections1/6/8 versus
optimization/FINAL_REPORT.md and optimization/BEST_RESULT.md. The old handoff
calls the74560a cumulative candidate current and directs restoration to
final-after. The later sealed conclusion rejected unproven candidates and keeps
the original5bce9e0/a918d84e as best. Following the old entry can select an
unqualified implementation for integration. Update the current-version entry
while preserving historical reports; require one unambiguous source/binary
identity for the next FTS delivery.

No official test, expected result, input, scorer or product file was changed by
this review. These are handoff requirements for the implementation owner; this
chat has not implemented fixes or sent remote comments.
