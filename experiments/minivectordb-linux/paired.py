#!/usr/bin/env python3
"""Compare known-good builds in balanced ABBA/BAAB blocks on one runner."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import runpy
import tempfile

from prepare import read, sha, write


SEED = 20261118
PLAN = {
    'default': (8, ('aa', 'I', 'F', 'R', 'IFOR', 'P')),
    'batch10000': (6, ('aa', 'I', 'R', 'IFOR', 'P')),
    'large-response': (8, ('aa', 'O', 'IFOR')),
    'shuffled-pk10': (6, ('aa', 'I', 'O')),
    'ascending-pk2500': (4, ('aa', 'O')),
    'ascending-pk2501': (4, ('aa', 'O')),
    'responses': (6, ('aa', 'IFOR', 'P')),
}

FOCUS = {
    'default': {'I': ('write_ms',), 'F': ('filtered_ms', 'first_query_ms', 'query_ms'),
                'R': ('recovery_warm_ms',), 'IFOR': ('write_ms', 'query_ms', 'recovery_warm_ms'),
                'P': ('write_ms', 'query_ms')},
    'batch10000': {'I': ('write_ms',), 'R': ('recovery_warm_ms',),
                   'IFOR': ('write_ms', 'query_ms', 'recovery_warm_ms'), 'P': ('write_ms', 'query_ms')},
    'large-response': {'O': ('large_ms', 'query_ms'), 'IFOR': ('large_ms', 'query_ms')},
    'shuffled-pk10': {'I': ('write_ms',), 'O': ('query_ms',)},
    'ascending-pk2500': {'O': ('query_ms',)},
    'ascending-pk2501': {'O': ('query_ms',)},
    'responses': {'IFOR': ('query_ms',), 'P': ('query_ms',)},
}


def ordered_read(driver, name, count, shuffled, queries):
    n, batch = 10000, 100
    driver['PROFILES'][name] = (n, 8, batch, queries, 'pk-order')
    keys = list(range(1, n + 1))
    if shuffled:
        random.Random(SEED).shuffle(keys)
    inserts, selects = [], []
    for start in range(0, n, batch):
        values = [f"({k},'c{k % 100}','[0,0,0,0,0,0,0,0]')" for k in keys[start:start + batch]]
        ident = len(inserts) + 2
        inserts.append((driver['message'](ident, 'INSERT INTO bench VALUES ' + ','.join(values) + ';'), ident, batch))
    expected = hashlib.sha256()
    for i in range(queries):
        ident = len(inserts) + 2 + i
        ascending = i % 2 == 0
        direction = 'ASC' if ascending else 'DESC'
        chosen = range(1, count + 1) if ascending else range(n, n - count, -1)
        sql = f'SELECT id,cat FROM bench ORDER BY id {direction} LIMIT {count};'
        selects.append((driver['message'](ident, sql), ident, 'unfiltered'))
        reply = {'id': ident, 'ok': True, 'columns': ['id', 'cat'], 'rows': [[k, f'c{k % 100}'] for k in chosen]}
        expected.update((json.dumps(reply, separators=(',', ':')) + '\n').encode())
    requests = hashlib.sha256(b''.join(m for m, _, _ in inserts + selects)).hexdigest()
    return (inserts, selects, requests), expected.hexdigest()


def workloads(driver, plan):
    controls = {'shuffled-pk10': (10, True, 96), 'ascending-pk2500': (2500, False, 32),
                'ascending-pk2501': (2501, False, 32)}
    inputs, expected = {}, {}
    for profile in plan:
        if profile in controls:
            inputs[profile], expected[profile] = ordered_read(driver, profile, *controls[profile])
        else:
            inputs[profile] = driver['workload'](profile)
    return inputs, expected


def schedule(plan):
    rng = random.Random(SEED)
    orders = {}
    for profile, (blocks, comparisons) in plan.items():
        assert blocks % 2 == 0 and 'aa' in comparisons
        for variant in comparisons:
            patterns = ['ABBA', 'BAAB'] * (blocks // 2)
            rng.shuffle(patterns)
            orders[profile, variant] = patterns
    for block in range(max(len(order) for order in orders.values())):
        pairs = [pair for pair, order in orders.items() if block < len(order)]
        rng.shuffle(pairs)
        for profile, variant in pairs:
            yield {'profile': profile, 'comparison': variant, 'block': block,
                   'order': orders[profile, variant][block]}


def choose_cpus():
    if not hasattr(os, 'sched_getaffinity'):
        return None, None
    cpus = sorted(os.sched_getaffinity(0))
    database = cpus[0]
    if len(cpus) == 1:
        return database, database

    def siblings(cpu):
        path = Path(f'/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list')
        return path.read_text().strip() if path.exists() else ''

    client = next((cpu for cpu in cpus[1:] if siblings(cpu) != siblings(database)), cpus[-1])
    return database, client


def perform(driver_path, builds, output, data_root, plan=PLAN):
    driver = runpy.run_path(str(driver_path))
    binaries = {name: spec['binary'] for name, spec in builds.items()}
    digests = {name: sha(Path(path)) for name, path in binaries.items()}
    assert digests == {name: spec['binary_sha256'] for name, spec in builds.items()}
    output.mkdir()
    inputs, expected = workloads(driver, plan)
    groups = list(schedule(plan))
    db_cpu, client_cpu = choose_cpus()
    if client_cpu is not None:
        os.sched_setaffinity(0, {client_cpu})
    write(output / 'metadata.json', {'seed': SEED, 'driver_sha256': sha(driver_path),
          'binaries_sha256': digests, 'profile_parameters': {p: driver['PROFILES'][p] for p in inputs},
          'input_sha256': {p: value[2] for p, value in inputs.items()}, 'independent_expected_query_digest': expected,
          'schedule': groups, 'database_cpu': db_cpu, 'client_cpu': client_cpu,
          'time_unit': 'ms', 'recovery': 'restart of own just-written fixture, warm OS cache'})
    records, signatures = [], {}
    (output / 'stderr').mkdir()
    with tempfile.TemporaryDirectory(prefix='mvd-data-', dir=data_root) as temp, (output / 'runs.jsonl').open('w') as raw:
        for group in groups:
            profile, variant = group['profile'], group['comparison']
            for arm in group['order']:
                name = 'B' if arm == 'A' or variant == 'aa' else variant
                run_id = len(records)
                try:
                    record = driver['run_once'](binaries[name], profile, inputs[profile], output, run_id, db_cpu, 120, temp)
                    signature = (record['query_digest'], record['wal_sha256'])
                    assert signatures.setdefault(profile, signature) == signature, signature
                    if profile in expected:
                        assert signature[0] == expected[profile], signature
                except Exception as error:
                    write(output / 'failure.json', {**group, 'run_id': run_id, 'arm': arm, 'variant': name,
                                                    'error': repr(error)})
                    raise
                record.update(**group, arm=arm, run_id=run_id, variant=name)
                records.append(record)
                raw.write(json.dumps(record) + '\n')
                raw.flush()
                for log in (output / 'stderr' / f'{run_id}.log', output / 'stderr' / f'{run_id}-reopen.log'):
                    if log.is_file() and log.stat().st_size == 0:
                        log.unlink()
            print(f'{profile} {variant} block={group["block"]} {group["order"]} ({len(records)} runs)', flush=True)
    assert len(records) == len(groups) * 4 and digests == {name: sha(Path(path)) for name, path in binaries.items()}
    summary = driver['summarize'](records, SEED)
    write(output / 'summary.json', summary)
    write(output / 'digests.json', signatures)
    return summary


def speedups(summary, focus):
    rows = []
    for profile, candidates in focus.items():
        for variant, metrics in candidates.items():
            for metric in metrics:
                sample = summary[profile][variant][metric]
                low, high = sample['block_bootstrap_95pct']
                threshold = sample['screen_threshold_pct']
                signal = 'faster' if high < -threshold else 'slower' if low > threshold else 'inconclusive'
                def factor(delta):
                    return 100 / (100 + delta)
                rows.append({'profile': profile, 'variant': variant, 'metric': metric, 'signal': signal,
                             'speedup': factor(sample['median_change_pct']),
                             'speedup_interval': [factor(high), factor(low)], **sample})
    return rows


def report(output, rows, host):
    write(output / 'speedups.json', rows)
    lines = ['# MiniVectorDB native Linux paired comparison', '',
             f"Runner: {host['cpu_model']}, Linux {host['kernel']}, AVX2/FMA: {host['avx2_fma']}.", '',
             'Speedup is baseline time divided by candidate time, from block log ratios. Intervals are descriptive '
             'paired block bootstraps. A directional result clears both 3% and the measured A/A p95.',
             '', '| Workload | Candidate | Phase | Speedup | 95% interval | A/A p95 | Screening |',
             '| --- | --- | --- | ---: | ---: | ---: | --- |']
    for row in rows:
        low, high = row['speedup_interval']
        lines.append(f"| {row['profile']} | {row['variant']} | {row['metric']} | {row['speedup']:.3f}× | "
                     f"{low:.3f}–{high:.3f}× | {row['aa_p95_absolute_change_pct']:.2f}% | {row['signal']} |")
    md = '\n'.join(lines) + '\n'
    (output / 'speedups.md').write_text(md)
    if os.environ.get('GITHUB_STEP_SUMMARY'):
        with Path(os.environ['GITHUB_STEP_SUMMARY']).open('a') as stream:
            stream.write(md)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--data-root', required=True, type=Path)
    args = parser.parse_args()
    assert __debug__
    output = args.output.resolve()
    builds = read(output / 'builds.json')['variants']
    correctness = read(output / 'correctness.json')
    assert builds.keys() == correctness.keys()
    assert all(row['passed'] and row['binary_sha256'] == builds[k]['binary_sha256']
               for k, row in correctness.items())
    summary = perform(Path(builds['B']['source']) / 'tests/paired_performance.py', builds, output / 'paired', args.data_root)
    report(output, speedups(summary, FOCUS), read(output / 'host.json'))


if __name__ == '__main__':
    main()
