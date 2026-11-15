#!/usr/bin/env bash
# Historical IEEE campaign (frozen a285f2e/9db50ce archives, not the CRC-32C integration).
# Execute from a fresh uploaded bundle containing input/{baseline,candidate} and harness/.
set -euo pipefail
case_root=$(cd "$1" && pwd)
server_cpu=${2:-2}
client_cpu=${3:-4}
cd "$case_root"
mkdir remote-logs
python3 - <<'PY'
import json,os,platform,subprocess
from pathlib import Path
Path('linux-machine.json').write_text(json.dumps({
    'platform':platform.platform(),'uname':list(os.uname()),'load':os.getloadavg(),
    'affinity':sorted(os.sched_getaffinity(0)),
    'lscpu':subprocess.check_output(['lscpu'],text=True),
    'filesystem':subprocess.check_output(['df','-T','.'],text=True),
    'memory':Path('/proc/meminfo').read_text()},indent=2)+'\n')
PY
python3 harness/tests/build_variants.py --baseline-source input/baseline \
  --candidate-source input/candidate --output linux-build
for variant in baseline crc buffer combined; do
  python3 harness/tests/validate.py --binary "linux-build/$variant/minivectordb" \
    --baseline linux-build/baseline/minivectordb --output "correctness-$variant" \
    --seeds 5 --rounds 500 --steps 1000 > "remote-logs/correctness-$variant.log" 2>&1
  printf '%s correctness passed\n' "$variant"
done
g++ -std=c++17 -O3 -Wall -Wextra -Ilinux-build/combined/source/src \
  harness/tests/crc32.cpp -o linux-build/crc32-test
linux-build/crc32-test > remote-logs/crc32.log 2>&1
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
  -Ilinux-build/combined/source/src harness/tests/crc32.cpp -o linux-build/crc32-sanitized
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  linux-build/crc32-sanitized > remote-logs/crc32-sanitized.log 2>&1
mkdir data
python3 harness/tests/paired_performance.py --binaries linux-build/binaries.json \
  --output linux-smoke --profiles smoke --blocks 1 --aa-blocks 1 \
  --cpu "$server_cpu" --client-cpu "$client_cpu" --data-root "$case_root/data"
python3 harness/tests/paired_performance.py --binaries linux-build/binaries.json \
  --output linux-paired --profiles default,batch1-1k,batch10000,dim8,responses,large-response \
  --blocks 6 --aa-blocks 10 --cpu "$server_cpu" --client-cpu "$client_cpu" \
  --data-root "$case_root/data"
printf 'All Linux correctness checks and paired measurements completed.\n'
