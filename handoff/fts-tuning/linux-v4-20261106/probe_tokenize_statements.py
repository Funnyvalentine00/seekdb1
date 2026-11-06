"""Run every original single-line SELECT even when a prior statement errors."""
from pathlib import Path
import hashlib
import json
import verify_linux as v

source = (v.R / 'tokenize.sql').read_text()
lines = source.splitlines()
assert lines[0] == 'SET NAMES utf8mb4;'
assert all(line.startswith('SELECT ') and line.endswith(';') for line in lines[1:])
out = v.O / 'tokenize-statements'
out.mkdir(exist_ok=False)
records = {}
for variant in ['v3', 'fixed']:
    records[variant] = []
    p, data, log = v.start(variant, 'tokenize-statements-' + variant)
    try:
        for lineno, query in enumerate(lines[1:], 2):
            result = v.sql(lines[0] + '\n' + query)
            stem = out / f'{variant}-line{lineno}'
            stem.with_suffix('.out').write_bytes(result.stdout)
            stem.with_suffix('.err').write_bytes(result.stderr)
            records[variant].append(dict(
                original_line=lineno, query=query, rc=result.returncode,
                stdout_sha256=hashlib.sha256(result.stdout).hexdigest(),
                stderr_sha256=hashlib.sha256(result.stderr).hexdigest()))
    finally:
        v.stop(p, log)
(out / 'summary.json').write_text(json.dumps(records, indent=2, ensure_ascii=False))
assert records['v3'] == records['fixed']
print(json.dumps(dict(statements_per_version=len(records['v3']),
                     success_per_version=sum(r['rc'] == 0 for r in records['v3']),
                     matched_error_lines=[r['original_line'] for r in records['v3'] if r['rc']],
                     all_outputs_statuses_equal=True)))
