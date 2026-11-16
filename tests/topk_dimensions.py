#!/usr/bin/env python3
"""Detect omitted coordinates in batched/cached TopK and stale rows after updates."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def run(binary):
    count = 0
    shapes = [(dim, extra_tail) for dim in (8, 31, 32, 33, 64, 96, 128)
              for extra_tail in ((False, True) if dim in (32, 64, 96, 128) else (False,))]
    for dim, extra_tail in shapes:
        vectors = {i + 1: [0] * i + [i % 3 + 1] + [0] * (dim - i - 1)
                   for i in range(dim)}
        # Independent, exact integer squared norms for the single-nonzero fixtures.
        distances = {i + 1: (i % 3 + 1) ** 2 for i in range(dim)}
        if extra_tail:
            # An odd physical slot count reaches the uncached scanner's final
            # row. Ignoring its last coordinate would incorrectly make it win.
            vectors[dim + 1] = [0] * (dim - 1) + [2]
            distances[dim + 1] = 4
        for cache in (True, False):
            env = os.environ.copy()
            env.pop('MVD_NOVECCACHE', None)
            if not cache:
                env['MVD_NOVECCACHE'] = '1'
            for mode in ('exact', 'ann'):
                steps = [
                    ('CREATE TABLE t (id BIGINT PRIMARY KEY, cat BIGINT, v VECTOR(%d));' % dim, {}),
                    ('INSERT INTO t VALUES ' + ','.join(
                        "(%d,1,'%s')" % (i, json.dumps(v)) for i, v in vectors.items()) + ';',
                     {'affected_rows': len(vectors)}),
                ]
                norms = dict(distances)
                zero = json.dumps([0] * dim)

                def checks():
                    ordered = sorted(norms, key=lambda i: (norms[i], i))
                    for where in ('', 'WHERE cat = 1', 'WHERE id > 0'):
                        for k in (1, 5, dim + 1):
                            steps.append((f"SELECT id FROM t {where} ORDER BY L2_DISTANCE(v, '{zero}') LIMIT {k};",
                                          {'columns': ['id'], 'rows': [[i] for i in ordered[:k]]}))

                checks()  # initial build and repeated query/cache reuse
                steps.append(('DELETE FROM t WHERE id = 2;', {'affected_rows': 1}))
                del norms[2]
                checks()  # holes and odd/even tails
                steps.append((f"UPDATE t SET v = '{zero}' WHERE id = {dim};", {'affected_rows': 1}))
                norms[dim] = 0
                checks()  # cache invalidation and reordered winner
                messages = [{'id': i, 'op': 'execute', 'sql': sql, 'search_mode': mode}
                            for i, (sql, _) in enumerate(steps)]
                messages.append({'id': len(steps), 'op': 'shutdown'})
                with tempfile.TemporaryDirectory(prefix='mvd-dimensions-') as directory:
                    result = subprocess.run([str(binary), '--stdio', '--data-dir='+directory],
                                            input=''.join(json.dumps(m)+'\n' for m in messages),
                                            text=True, capture_output=True, env=env, timeout=30)
                assert result.returncode == 0, result.stderr
                actual = [json.loads(line) for line in result.stdout.splitlines()]
                assert len(actual) == len(messages)+1, (dim, cache, mode, actual)
                assert actual[0] == {'type': 'ready'}
                for i, (sql, expected) in enumerate(steps):
                    want = {'id': i, 'ok': True, **expected}
                    assert actual[i+1] == want, (dim, extra_tail, cache, mode, sql, want, actual[i+1])
                assert actual[-1] == {'id': len(steps), 'ok': True}
                count += 1
    print(f'TopK every-coordinate, cache on/off, exact/ann, delete/update: {count} sessions passed')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    run(parser.parse_args().binary.resolve())
