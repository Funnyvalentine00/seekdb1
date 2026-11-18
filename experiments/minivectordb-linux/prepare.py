#!/usr/bin/env python3
"""Materialize hash-pinned source trees and run their native correctness gates."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import resource
import subprocess
import sys


HERE = Path(__file__).resolve().parent


def read(path):
    return json.loads(path.read_text())


def write(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def capture(command, cwd=None):
    return subprocess.check_output(command, cwd=cwd, text=True).strip()


def logged(command, log):
    with log.open('a') as stream:
        print(json.dumps(command), file=stream, flush=True)
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        print(log.read_text(errors='replace'), file=sys.stderr)
        result.check_returncode()


def record_host(out):
    assert sys.platform == 'linux' and platform.machine() == 'x86_64' and hasattr(resource, 'prlimit')
    lscpu = capture(['lscpu'])
    (out / 'lscpu.txt').write_text(lscpu + '\n')
    flags = next(line.split(':', 1)[1].split() for line in Path('/proc/cpuinfo').read_text().splitlines()
                 if line.startswith('flags'))
    host = {'kernel': platform.release(), 'machine': platform.machine(), 'python': platform.python_version(),
            'compiler': capture(['g++', '--version']).splitlines()[0], 'image': os.getenv('ImageOS'),
            'image_version': os.getenv('ImageVersion'), 'commit': os.getenv('GITHUB_SHA'),
            'run_id': os.getenv('GITHUB_RUN_ID'), 'run_attempt': os.getenv('GITHUB_RUN_ATTEMPT'),
            'cpu_model': next(line.split(':', 1)[1].strip() for line in lscpu.splitlines()
                              if line.startswith('Model name:')),
            'allowed_cpus': sorted(os.sched_getaffinity(0)), 'flags': sorted(flags),
            'avx2_fma': {'avx2', 'fma'}.issubset(flags), 'sse42': 'sse4_2' in flags}
    write(out / 'host.json', host)
    print({k: host[k] for k in ('kernel', 'cpu_model', 'allowed_cpus', 'avx2_fma', 'sse42')}, flush=True)


def build(args):
    spec, out = read(HERE / 'sources.json'), args.output.resolve()
    out.mkdir()
    scratch = args.scratch.resolve()
    scratch.mkdir()
    if not args.trees_only:
        record_host(out)
    repo = args.repo.resolve()
    assert capture(['git', 'rev-parse', spec['base_commit'] + '^{tree}'], repo) == spec['trees']['B']
    archive = scratch / 'baseline.tar'
    subprocess.run(['git', 'archive', '-o', archive, spec['base_commit']], cwd=repo, check=True)
    for patch, expected in spec['patches'].items():
        assert sha(HERE / 'patches' / (patch + '.patch')) == expected, patch
    variants = {}
    for name, patches in spec['variants'].items():
        source = scratch / name
        source.mkdir()
        subprocess.run(['tar', '-xf', archive, '-C', source], check=True)
        for patch in patches:
            subprocess.run(['git', '-c', 'core.autocrlf=false', 'apply', '--whitespace=nowarn',
                            HERE / 'patches' / (patch + '.patch')], cwd=source, check=True)
        subprocess.run(['git', 'init', '-q', '-b', 'snapshot'], cwd=source, check=True)
        subprocess.run(['git', '-c', 'core.autocrlf=false', '-c', 'core.filemode=true',
                        'add', '--force', '--all'], cwd=source, check=True)
        tree = capture(['git', 'write-tree'], source)
        assert tree == spec['trees'][name], (name, tree)
        variants[name] = {'source': str(source), 'tree': tree, 'patches': patches}
        print(name, tree, flush=True)
        if not args.trees_only:
            target = scratch / (name + '-build')
            log = out / ('build-' + name + '.log')
            logged(['cmake', '-S', str(source), '-B', str(target), '-DCMAKE_BUILD_TYPE=Release',
                    '-DCMAKE_CXX_COMPILER=g++'], log)
            logged(['cmake', '--build', str(target), '--parallel', '4'], log)
            binary = target / 'minivectordb'
            variants[name].update(binary=str(binary), binary_sha256=sha(binary))
        write(out / 'builds.json', {'base_commit': spec['base_commit'], 'variants': variants})


def validate(args):
    out = args.output.resolve()
    versions = read(out / 'builds.json')['variants']
    reports = {}
    for name, spec in versions.items():
        print(f'::group::Linux correctness and memory {name}', flush=True)
        source, binary = Path(spec['source']), Path(spec['binary'])
        assert sha(binary) == spec['binary_sha256']
        cases = out / ('validate-' + name)
        extended = name in ('B', 'IFOR')
        command = [sys.executable, str(source / 'tests/validate.py'), '--binary', str(binary),
                   '--baseline', versions['B']['binary'], '--output', str(cases),
                   '--seeds', '3' if extended else '1', '--rounds', '500' if extended else '200',
                   '--steps', '1000' if extended else '500', '--large-rows', '200000' if extended else '25000']
        result = subprocess.run(command)
        summary_path = cases / 'summary.json'
        report = read(summary_path) if summary_path.exists() else {}
        counts = {r['name']: r['status'] for r in report.get('checks', [])}
        required = ('topk-memory', 'filtered-topk-memory')
        passed = result.returncode == 0 and report.get('passed') and not report['skipped']
        passed = bool(passed and all(counts.get(k) == 'passed' for k in required))
        reports[name] = {'passed': passed, 'binary_sha256': spec['binary_sha256'], 'counts': counts}
        write(out / 'correctness.json', reports)
        print('::endgroup::', flush=True)
    if not all(r['passed'] for r in reports.values()):
        raise SystemExit('One or more Linux correctness suites failed. See the saved logs.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(required=True)
    pre = commands.add_parser('build')
    for flag in ('repo', 'scratch', 'output'):
        pre.add_argument('--' + flag, type=Path, required=True)
    pre.add_argument('--trees-only', action='store_true')
    pre.set_defaults(run=build)
    correctness = commands.add_parser('validate')
    correctness.add_argument('--output', type=Path, required=True)
    correctness.set_defaults(run=validate)
    args = parser.parse_args()
    assert __debug__
    args.run(args)


if __name__ == '__main__':
    main()
