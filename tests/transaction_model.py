#!/usr/bin/env python3
"""Compare transactional PK mutation and crash recovery with SQLite's row model.

SQLite is a test oracle only, not a dependency of the database. Generated SQL
uses integer columns and supported CRUD; compare success/failure and full rows,
not SQLite error names. Each failure retains the seed and complete SQL trace.
"""
import argparse
import json
from pathlib import Path
import random
import select
import sqlite3
import subprocess
import tempfile


class Database:
    def __init__(self, binary, directory):
        self.binary, self.directory = binary, directory
        self.ident = 0
        self.proc = None
        self.start()

    def start(self):
        self.stderr = tempfile.TemporaryFile()
        self.proc = subprocess.Popen(
            [self.binary, "--stdio", "--data-dir=" + self.directory],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.stderr)
        assert self.read() == {"type": "ready"}

    def read(self):
        ready, _, _ = select.select([self.proc.stdout], [], [], 30)
        if not ready:
            raise TimeoutError("database response timeout")
        line = self.proc.stdout.readline()
        if not line:
            self.stderr.seek(0)
            raise RuntimeError("database closed stdout: " + self.stderr.read().decode(errors="replace"))
        return json.loads(line)

    def sql(self, sql):
        self.ident += 1
        self.proc.stdin.write((json.dumps({"id": self.ident, "op": "execute", "sql": sql}) + "\n").encode())
        self.proc.stdin.flush()
        result = self.read()
        assert result["id"] == self.ident, result
        return result

    def stop(self, crash=False):
        if self.proc is None:
            return
        if crash:
            self.proc.kill()
        else:
            self.proc.stdin.close()
        try:
            code = self.proc.wait(timeout=30)
            if not crash:
                assert code == 0, code
        finally:
            if self.proc.poll() is None:
                self.proc.kill()
                self.proc.wait()
            if not self.proc.stdin.closed:
                self.proc.stdin.close()
            self.proc.stdout.close()
            self.stderr.close()
            self.proc = None


def run(binary, seed, steps, trace_path):
    rng = random.Random(seed)
    oracle = sqlite3.connect(":memory:", isolation_level=None)
    trace = []
    checks = errors = crashes = 0
    with tempfile.TemporaryDirectory(prefix="mvd-tx-model-") as directory:
        db = Database(binary, directory)
        try:
            def execute(sql):
                nonlocal checks, errors
                trace.append(sql)
                expected_ok = True
                try:
                    oracle.execute(sql)
                except sqlite3.IntegrityError:
                    expected_ok = False
                result = db.sql(sql)
                assert result.get("ok") is expected_ok, (sql, expected_ok, result)
                errors += not expected_ok
                wanted = [list(row) for row in oracle.execute("SELECT id, value FROM t ORDER BY id")]
                actual = db.sql("SELECT id, value FROM t ORDER BY id;")
                assert actual.get("ok") is True and actual.get("rows") == wanted, (sql, wanted, actual)
                checks += 1

            execute("CREATE TABLE t (id BIGINT PRIMARY KEY, value BIGINT);")
            execute("INSERT INTO t VALUES (-9223372036854775808,0),(9223372036854775807,1);")
            in_txn = False
            for step in range(steps):
                if not in_txn and rng.random() < 0.2:
                    execute("BEGIN;")
                    in_txn = True
                ids = [row[0] for row in oracle.execute("SELECT id FROM t ORDER BY id")]
                old = rng.choice(ids) if ids and rng.random() < 0.8 else rng.randrange(-20, 21)
                new = rng.randrange(-20, 21)
                value = rng.randrange(-100000, 100001)
                action = rng.randrange(5)
                if action == 0:
                    execute(f"INSERT INTO t VALUES ({new},{value});")
                elif action == 1:
                    # Both success and duplicate-key rejection must be atomic.
                    second = new if rng.random() < 0.3 else rng.randrange(-20, 21)
                    execute(f"INSERT INTO t VALUES ({new},{value}),({second},{-value});")
                elif action == 2:
                    execute(f"UPDATE t SET id={new}, value={value} WHERE id={old};")
                elif action == 3:
                    execute(f"DELETE FROM t WHERE id={old};")
                else:
                    execute(f"UPDATE t SET value={value} WHERE id <= {new};")
                if rng.random() < 0.08:
                    trace.append("-- SIGKILL and reopen (uncommitted rows must disappear)")
                    db.stop(crash=True)
                    if in_txn:
                        oracle.execute("ROLLBACK;")
                        in_txn = False
                    db.start()
                    execute("SELECT id FROM t;")
                    crashes += 1
                elif in_txn and rng.random() < 0.3:
                    execute(rng.choice(["COMMIT;", "ROLLBACK;"]))
                    in_txn = False
            if in_txn:
                execute("COMMIT;")
            db.stop()
            db.start()
            execute("SELECT id FROM t;")
        except BaseException:
            trace_path.parent.mkdir(parents=True, exist_ok=True)
            trace_path.write_text(f"-- seed={seed}, steps={steps}\n" + "\n".join(trace) + "\n")
            raise
        finally:
            db.stop()
            oracle.close()
    print(json.dumps({"seed": seed, "steps": steps, "state_checks": checks,
                      "expected_constraint_errors": errors, "crash_restarts": crashes}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="./build/minivectordb")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--steps", type=int, default=1000)
    parser.add_argument("--failure-trace", type=Path, default=Path("transaction-failure.sql"))
    args = parser.parse_args()
    if args.steps < 1:
        parser.error("--steps must be positive")
    run(str(Path(args.binary).resolve()), args.seed, args.steps, args.failure_trace)
