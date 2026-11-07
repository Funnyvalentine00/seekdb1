#!/usr/bin/env bash
set -euo pipefail
set -o noclobber
case "$1" in red|shadow-red|shadow-green) ;; *) exit 64 ;; esac
cd /home/xiazihan/benchmark/fts-v4-integration-20261106/correctness-fixes-20261107-134703
set +e
python3 -B run_cases.py "$1" > "$1.log" 2>&1
task_rc=$?
set -e
printf '%s\n' "$task_rc" > "$1.rc"
exit "$task_rc"
