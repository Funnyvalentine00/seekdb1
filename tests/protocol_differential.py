#!/usr/bin/env python3
"""Compare a seeded JSON Lines workload and restart across two binaries."""

import argparse
import hashlib
import json
from pathlib import Path
import random
import subprocess
import tempfile


def request(raw_id, **fields):
    body = json.dumps(fields, ensure_ascii=False)
    return f'{{"id":{raw_id},{body[1:]}'


def request_id(rng, index):
    return rng.choice([
        str(10**30 + index), f"{index}.00e+01", "null", "true",
        json.dumps(f'请求 {index} "雪"\n😀', ensure_ascii=False),
        json.dumps({"request": [index, None, "雪"]}),
        json.dumps([index, False, "\\path"]),
    ])


def make_workload(seed, count):
    rng = random.Random(seed)
    rows = ",".join(f"({i},'group_{i % 4}','[{i % 7},{i * 2 % 9}]')"
                    for i in range(1, 97))
    requests = [
        request("1", op="execute", sql="CREATE TABLE records "
                "(id BIGINT PRIMARY KEY, label VARCHAR(48), v VECTOR(2));"),
        request("2", op="execute", sql=f"INSERT INTO records VALUES {rows};"),
    ]
    large = "a雪" * 256
    for index in range(count):
        raw_id = request_id(rng, index)
        position = index % 120
        if position == 0:
            # A short request produces over 80 KiB by projecting the literal on 80 rows.
            sql = f"SELECT '{large}' AS payload FROM records ORDER BY id LIMIT 80;"
        elif position == 1:
            sql = "SELECT id FROM records WHERE id = 1;"
        elif position == 2:
            requests.append(f'{{"id":{raw_id},"op":')
            continue
        elif position == 3:
            sql = "SELECT L2_DISTANCE(v, '[1e38,0]') FROM records WHERE id = 1;"
        elif position == 4:
            requests.append(request(raw_id, op="execute", sql="SELECT id FROM records;")
                            + " trailing")
            continue
        else:
            key = rng.randint(100, 132)
            limit = rng.randint(5, 24)
            group = rng.randrange(4)
            sql = rng.choice([
                f"SELECT id, label, v FROM records ORDER BY id LIMIT {limit};",
                f"SELECT id, label FROM records WHERE label = 'group_{group}' ORDER BY id;",
                "SELECT id, L2_DISTANCE(v, '[1,2]') AS d FROM records ORDER BY d LIMIT 10;",
                "SELECT id, L2_DISTANCE(v, '[2,3]') AS d FROM records "
                f"WHERE label = 'group_{group}' ORDER BY d LIMIT 10;",
                f"SELECT id FROM records WHERE id = {key};",
                f"INSERT INTO records VALUES ({key}, '雪😀 {key}', '[3,4]');",
                f"UPDATE records SET label = 'updated {key}' WHERE id = {key};",
                f"DELETE FROM records WHERE id = {key};",
                "SELECT unknown_column FROM records;",
                "SELECT id FROM missing_table;",
                "UPDATE records SET v = '[1]' WHERE id = 1;",
            ])
        requests.append(request(raw_id, op="execute", sql=sql,
                                search_mode=rng.choice(["exact", "ann"])))
    requests.append(request('"end"', op="shutdown"))
    return requests


def encode(requests):
    return ("\n".join(requests) + "\n").encode()


def reject_constant(value):
    raise AssertionError(f"non-finite JSON number: {value}")


def check_phase(binary, directory, requests, shutdown_id):
    output = subprocess.run([binary, "--stdio", f"--data-dir={directory}"],
                            input=encode(requests), capture_output=True, timeout=60)
    assert output.returncode == 0, (binary, output.returncode, output.stderr[:4000])
    assert not output.stderr, (binary, output.stderr[:4000])
    lines = output.stdout.split(b"\n")
    assert lines[-1] == b"" and len(lines) == len(requests) + 2, (binary, len(lines))
    for index, line in enumerate(lines[:-1]):
        response = json.loads(line, parse_constant=reject_constant)
        if index == 0:
            assert response == {"type": "ready"}, (binary, response)
        else:
            assert (isinstance(response, dict) and "id" in response and
                    type(response.get("ok")) is bool), (binary, index, line[:100])
    assert response == {"id": shutdown_id, "ok": True}, (binary, response)
    return lines[:-1]


def compare(label, baseline, candidate):
    for index, (left, right) in enumerate(zip(baseline, candidate)):
        if left != right:
            raise AssertionError(f"{label} response {index}: baseline ({len(left)} B) "
                                 f"{left[:160]!r}, candidate ({len(right)} B) {right[:160]!r}")


def run(baseline, candidate, seed, count):
    requests = make_workload(seed, count)
    restart = [
        request('"reopen"', op="execute", sql="SELECT id, label, v FROM records ORDER BY id;"),
        request('"done"', op="shutdown"),
    ]
    try:
        with tempfile.TemporaryDirectory() as baseline_dir, tempfile.TemporaryDirectory() as candidate_dir:
            expected = check_phase(baseline, baseline_dir, requests, "end")
            actual = check_phase(candidate, candidate_dir, requests, "end")
            compare("workload", expected, actual)
            large_responses = sum(len(line) > 80 * 1024 for line in actual)
            assert large_responses == (count + 119) // 120, (large_responses, count)
            # Check each intentional transition after a large response.
            for index in range(0, count, 120):
                start = index + 3  # ready and two setup replies
                stop = min(index + 5, count) + 3
                assert len(actual[start]) > 80 * 1024, (index, actual[start][:100])
                assert [json.loads(line)["ok"] for line in actual[start:stop]] == \
                    [True, True, False, False, False][:stop - start], index
            expected_restart = check_phase(baseline, baseline_dir, restart, "done")
            actual_restart = check_phase(candidate, candidate_dir, restart, "done")
            compare("restart", expected_restart, actual_restart)
            reopened = json.loads(actual_restart[1])
            assert (reopened["ok"] and isinstance(reopened.get("rows"), list) and
                    len(reopened["rows"]) >= 96), actual_restart[1][:100]
            assert reopened["rows"][0] == [1, "group_1", [1, 2]], reopened["rows"][0]
    except (AssertionError, ValueError, OSError, subprocess.SubprocessError):
        failure_dir = Path(tempfile.mkdtemp(prefix=f"protocol-differential-{seed}-"))
        (failure_dir / "workload.jsonl").write_bytes(encode(requests))
        (failure_dir / "restart.jsonl").write_bytes(encode(restart))
        print(f"seed={seed} requests={count} corpus={failure_dir}", flush=True)
        raise
    digest = hashlib.sha256(b"\n".join(actual + actual_restart) + b"\n").hexdigest()
    print(f"seed={seed} requests={len(requests)} large_responses={large_responses} "
          f"identical_sha256={digest} restart_rows={len(reopened['rows'])}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--candidate", required=True)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--requests", type=int, default=3000,
                        help="generated requests, in addition to setup and shutdown")
    args = parser.parse_args()
    if args.requests < 1:
        parser.error("--requests must be positive")
    run(args.baseline, args.candidate, args.seed, args.requests)
