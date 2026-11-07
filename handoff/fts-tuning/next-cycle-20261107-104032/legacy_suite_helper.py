"""Selected read/run helpers from the archived Linux reference driver."""
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

E = dict(os.environ)


def rss(pid):
    value = Path(f'/proc/{pid}/status').read_text()
    return {m[1]: int(m[2]) * 1024
            for m in re.finditer(r'^(VmRSS|VmHWM):\s+(\d+) kB$', value, re.M)}

def capture_semantics(variant):
    records = {}
    for case in ['projection', 'score-controls', 'tokenize']:
        result = sql((R / (case + '.sql')).read_text(), force=True)
        (O / f'{variant}-{case}.out').write_bytes(result.stdout)
        (O / f'{variant}-{case}.err').write_bytes(result.stderr)
        records[case] = dict(rc=result.returncode,
                             stdout_sha256=hashlib.sha256(result.stdout).hexdigest(),
                             stderr_sha256=hashlib.sha256(result.stderr).hexdigest())
    (O / (variant + '-semantics.json')).write_text(json.dumps(records, indent=2))
    return records

def edge_probes(variant, pid):
    cases = json.loads((R / 'edge-cases.json').read_text())
    records = {}
    for name, (setup, query) in cases.items():
        a = sql('DROP DATABASE IF EXISTS edges;' + setup)
        assert a.returncode == 0, (name, a.stderr)
        b = sql(query)
        (O / f'edge-{variant}-{name}.out').write_bytes(b.stdout)
        (O / f'edge-{variant}-{name}.err').write_bytes(b.stderr)
        records[name] = dict(rc=b.returncode, rss_after=rss(pid),
                             stdout_sha256=hashlib.sha256(b.stdout).hexdigest(),
                             stderr_sha256=hashlib.sha256(b.stderr).hexdigest())
        print('EDGE', variant, name, b.returncode, flush=True)
        assert b.returncode == 0, (name, b.stderr)
    (O / (variant + '-edges.json')).write_text(json.dumps(records, indent=2))
    return records

def suite(variant, data, name):
    source = S / 'tools/deploy/mysql_test/test_suite' / name
    deploy = S / 'tools/deploy'
    out = O / 'regress' / variant / name
    out.mkdir(parents=True, exist_ok=True)
    env = dict(E, OBMYSQL_MS0='127.0.0.1', OBMYSQL_PORT=str(PORT), OBMYSQL_PWD='',
               OBMYSQL_USR='root', TENANT='mysql', OBSERVER_DIR=str(data),
               IS_BUSINESS='0', PWD=str(deploy))
    init = sql('CREATE DATABASE IF NOT EXISTS test;')
    assert init.returncode == 0, init.stderr
    records = []
    for original in sorted((source / 't').glob('*.test')):
        text = original.read_text()
        adapters = []
        if name == 'file_sql':
            fixture = data / 'data_for_mysqltest_file_sql'
            fixture.mkdir(exist_ok=False)
            for src in (source / 'data').iterdir():
                if src.is_file():
                    (fixture / src.name).write_bytes(src.read_bytes())
            encoded = fixture / 'simple.parquet.b64'
            (fixture / 'simple.parquet').write_bytes(base64.b64decode(encoded.read_bytes()))
            encoded.unlink()
            for include in ['copy_file_sql_data', 'remove_file_sql_data']:
                text = text.replace('--source mysql_test/test_suite/file_sql/include/' + include + '.inc', '')
            adapters.append('Local fixture deployment replaces SSH/scp only; original SQL and golden preserved')
        adapted = re.sub(r'(connect\s*\([^,]+,\s*\$OBMYSQL_MS0,)admin,', r'\1root,', text)
        if adapted != text:
            adapters.append('Use existing root instead of creating admin account')
            text = adapted
        actual = original
        if adapters:
            actual = out / original.name
            actual.write_text(text)
        args = [MT, '--host=127.0.0.1', f'--port={PORT}', '--user=root', '--database=test',
                '--silent', f'--tmpdir={out}', f'--logdir={out}', '--tail-lines=25',
                f'--test-file={actual}', f'--result-file={source}/r/mysql/{original.stem}.result']
        with (out / (original.stem + '.log')).open('w') as log:
            result = subprocess.run(args, cwd=deploy, env=env, stdout=log,
                                    stderr=subprocess.STDOUT, timeout=600)
        record = dict(case=original.stem, rc=result.returncode, adapters=adapters)
        records.append(record)
        print('SUITE', variant, name, original.stem, result.returncode, flush=True)
    (out / 'summary.json').write_text(json.dumps(records, indent=2))
    return records
