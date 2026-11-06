#!/usr/bin/env bash
set -euo pipefail
cd /home/xiazihan/benchmark/fts-v4-integration-20261106
while [ ! -f logs/build.rc ]; do sleep 5; done
if [ "$(cat logs/build.rc)" != 0 ] || [ ! -f logs/build.complete.json ]; then echo BUILD_NOT_COMPLETE; exit 1; fi
completed=0
on_exit() { rc=$?; if [ "$completed" != 1 ] && [ "$rc" = 0 ]; then rc=130; fi; echo "$rc" > logs/validation.rc; exit "$rc"; }
trap on_exit EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
python3 - <<'CHECK'
from pathlib import Path
import hashlib,json
r=Path.cwd();m=json.loads((r/'logs/build.complete.json').read_text())
assert set(m)=={'v3','fixed'}
for v,h in m.items():assert hashlib.sha256((r/'saved-binaries'/('seekdb-'+v)).read_bytes()).hexdigest()==h
CHECK
python3 verify_linux.py accept
python3 verify_linux.py aa
python3 verify_linux.py ab
completed=1
