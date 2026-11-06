from pathlib import Path
import base64
import hashlib
import json
import os
import re
import subprocess
import sys
import threading
import shutil
import time

R = Path('/home/xiazihan/benchmark/fts-v4-integration-20261106')
S = R / 'src'
O = R / 'results'
CLIENT = S / 'deps/3rd/u01/obclient/bin'
M = str(CLIENT / 'obclient')
MT = str(CLIENT / 'mysqltest')
PORT = 2917
E = dict(os.environ)
E['PATH'] = str(CLIENT) + ':' + E['PATH']
EXPECTED = dict(query_cn_hits=8001, query_beng_hits=11000,
                query_mixed_hits=7332, query_limit_hits=20)


def sql(query, force=False):
    return subprocess.run([M, '-h127.0.0.1', f'-P{PORT}', '-uroot', '-N', '-s',
                           '-B', '--default-character-set=utf8mb4'] +
                          (['--force'] if force else []) + ['-e', query],
                          capture_output=True, timeout=180, env=E)


def rss(pid):
    value = Path(f'/proc/{pid}/status').read_text()
    return {m[1]: int(m[2]) * 1024
            for m in re.finditer(r'^(VmRSS|VmHWM):\s+(\d+) kB$', value, re.M)}


def start(variant, tag):
    subprocess.run([sys.executable, str(R / 'resource_guard.py')], check=True)
    data = R / 'data' / tag
    data.mkdir(exist_ok=False)
    (data / '.owned-v4-validation').write_text(tag)
    log = (O / (tag + '-server.log')).open('w')
    p = subprocess.Popen([str(R / 'saved-binaries' / ('seekdb-' + variant)),
                          '--nodaemon', '--port', str(PORT), '--base-dir', str(data),
                          '--parameter', 'datafile_size=2G',
                          '--parameter', 'datafile_maxsize=2G',
                          '--parameter', 'memory_budget=2G',
                          '--parameter', 'vector_memory_limit=512M',
                          '--parameter', 'log_disk_size=2G'],
                         cwd=data, env=E, stdout=log, stderr=subprocess.STDOUT)
    try:
        for _ in range(240):
            if p.poll() is not None:
                raise RuntimeError('server exited: ' + tag)
            if sql('select 1').returncode == 0:
                return p, data, log
            time.sleep(.5)
        raise RuntimeError('startup timeout: ' + tag)
    except BaseException:
        stop(p, log)
        raise


def stop(p, log):
    if p.poll() is None:
        p.terminate()
        try:
            p.wait(timeout=40)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
    log.close()
    data = Path(p.args[p.args.index('--base-dir') + 1])
    assert data.parent.resolve() == (R / 'data').resolve() and not data.is_symlink()
    assert (data / '.owned-v4-validation').is_file()
    diagnostics = O / 'server-log-tails' / data.name
    diagnostics.mkdir(parents=True, exist_ok=True)
    for source in (data / 'log').glob('*'):
        if source.is_file() and not source.is_symlink():
            with source.open('rb') as f:
                f.seek(max(0, source.stat().st_size - 524288))
                (diagnostics / source.name).write_bytes(f.read())
    shutil.rmtree(data)
    print('OWN_DATA_RECLAIMED', data.name, flush=True)


def bench(tag, pid):
    env = dict(E, MYSQL=f'{M} -h127.0.0.1 -P{PORT} -uroot -N -s --default-character-set=utf8mb4',
               MYSQL_VERBOSE=f'{M} -h127.0.0.1 -P{PORT} -uroot --default-character-set=utf8mb4',
               LABEL=tag, ROWS='20000', BATCH='500', ROUNDS='3000', QUERY_ROUNDS='200',
               SAMPLES='3', WARMUP='30', SKIP_LOAD='0')
    done = threading.Event()
    peak = {'VmRSS': 0, 'VmHWM': 0}

    def poll():
        while not done.is_set():
            try:
                for key, value in rss(pid).items():
                    peak[key] = max(peak[key], value)
            except (FileNotFoundError, ProcessLookupError):
                pass
            done.wait(.2)

    thread = threading.Thread(target=poll, daemon=True)
    thread.start()
    print('BENCH_START', tag, flush=True)
    try:
        with (O / (tag + '.txt')).open('w') as out, (O / (tag + '.stderr')).open('w') as err:
            subprocess.run(['bash', str(S / 'tools/benchmark/fts_large_bench.sh')],
                           env=env, stdout=out, stderr=err, check=True, timeout=600)
    finally:
        done.set()
        thread.join()
        (O / (tag + '-rss.json')).write_text(json.dumps(peak, indent=2))
    metrics = {m[1]: float(m[2]) for m in re.finditer(
        r'^([A-Za-z0-9_]+):\s*([0-9.]+)\s*$', (O / (tag + '.txt')).read_text(), re.M)}
    assert all(metrics[k] == v for k, v in EXPECTED.items()), metrics
    print('BENCH_DONE', tag, flush=True)
    return metrics


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


def acceptance():
    summary = {}
    for variant in ['v3', 'fixed']:
        p, data, log = start(variant, 'check-' + variant)
        try:
            bench('check-' + variant, p.pid)
            semantics = capture_semantics(variant)
            edges = edge_probes(variant, p.pid)
            regress = {name: suite(variant, data, name)
                       for name in ['fts_index', 'file_sql', 'ai_column']}
            summary[variant] = dict(semantics=semantics, edges=edges, regression=regress)
            (O / 'acceptance-summary.json').write_text(json.dumps(summary, indent=2))
        finally:
            stop(p, log)
    assert summary['v3']['semantics'] == summary['fixed']['semantics']
    for case in summary['v3']['edges']:
        for key in ['rc', 'stdout_sha256', 'stderr_sha256']:
            assert summary['v3']['edges'][case][key] == summary['fixed']['edges'][case][key], (case, key)
    print('SEMANTICS_AND_EDGES_EQUAL', flush=True)


def ab():
    # Six pairs, three of each order, declared before collecting Linux results.
    for pair in range(1, 7):
        for variant in (['v3', 'fixed'] if pair % 2 else ['fixed', 'v3']):
            tag = f'ab{pair}-{variant}'
            p, data, log = start(variant, tag)
            try:
                bench(tag, p.pid)
            finally:
                stop(p, log)
    print('AB_COMPLETE', flush=True)


def aa():
    for label in ['a', 'b']:
        tag = 'aa-v3-' + label
        p, data, log = start('v3', tag)
        try:
            bench(tag, p.pid)
        finally:
            stop(p, log)
    print('AA_COMPLETE', flush=True)


if __name__ == '__main__':
    O.mkdir(exist_ok=True)
    if sys.argv[1] == 'accept':
        acceptance()
    elif sys.argv[1] == 'ab':
        ab()
    elif sys.argv[1] == 'aa':
        aa()
    else:
        raise SystemExit('Usage: verify_linux.py accept|aa|ab')
