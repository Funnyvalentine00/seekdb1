#!/usr/bin/env bash
set -euo pipefail
set -o noclobber
cd /home/xiazihan/benchmark/fts-v4-integration-20261106/retest-20261107-130006
set +e
python3 -B run_calibration.py > run.log 2>&1
task_rc=$?
set -e
printf '%s\n' "$task_rc" > run.rc
exit "$task_rc"
