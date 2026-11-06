#!/usr/bin/env bash
set -euo pipefail
RUN=/home/xiazihan/benchmark/fts-v4-integration-20261106
cd "$RUN"
completed=0
on_exit() {
  rc=$?
  if [ "$completed" != 1 ] && [ "$rc" = 0 ]; then rc=130; fi
  echo "$rc" > "$RUN/logs/build.rc"
  exit "$rc"
}
trap on_exit EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
python3 resource_guard.py
cd src
export RUSTUP_HOME=/home/xiazihan/benchmark/seekdb-verify-20261104/toolchain/rustup
export CARGO_HOME=/home/xiazihan/benchmark/seekdb-verify-20261104/toolchain/cargo
export CARGO_NET_OFFLINE=true CARGO_BUILD_JOBS=2 CMAKE_BUILD_PARALLEL_LEVEL=2
export PATH="$CARGO_HOME/bin:$PATH"
python3 ../validate_source.py v3
./build.sh release -DCMAKE_PROJECT_INCLUDE="$RUN/conservative_link.cmake" --make -j2 > ../logs/build-v3-resume1.log 2>&1
cp build_release/src/observer/seekdb ../saved-binaries/seekdb-v3
sha256sum ../saved-binaries/seekdb-v3 > ../saved-binaries/seekdb-v3.sha256
echo V3_BUILD_DONE
python3 ../resource_guard.py
patch -p1 < ../v3-to-fixed.patch
python3 ../validate_source.py fixed
./build.sh release -DCMAKE_PROJECT_INCLUDE="$RUN/conservative_link.cmake" --make -j2 > ../logs/build-fixed-resume1.log 2>&1
cp build_release/src/observer/seekdb ../saved-binaries/seekdb-fixed
sha256sum ../saved-binaries/seekdb-fixed > ../saved-binaries/seekdb-fixed.sha256
python3 - <<'CHECK'
from pathlib import Path
import hashlib,json,os
r=Path('/home/xiazihan/benchmark/fts-v4-integration-20261106');out={}
for variant in ['v3','fixed']:
 p=r/'saved-binaries'/('seekdb-'+variant)
 assert p.stat().st_size>10000000 and os.access(p,os.X_OK)
 h=hashlib.sha256(p.read_bytes()).hexdigest();assert h==(p.with_name(p.name+'.sha256')).read_text().split()[0];out[variant]=h
(r/'logs/build.complete.json').write_text(json.dumps(out,indent=2))
CHECK
completed=1
echo FIXED_BUILD_DONE
