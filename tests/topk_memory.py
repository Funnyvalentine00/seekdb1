#!/usr/bin/env python3
"""On Linux, large LIMIT must not reserve memory for deleted rows."""

import argparse
import json
from pathlib import Path
import resource
import select
import subprocess
import tempfile


class Session:
    def __init__(self, binary, directory):
        self.proc = subprocess.Popen(
            [binary, "--stdio", "--data-dir=" + directory],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True)
        self.ident = 0
        try:
            assert self.read() == {"type": "ready"}
        except BaseException:
            self.close()
            raise

    def read(self):
        readable, _, _ = select.select([self.proc.stdout], [], [], 30)
        assert readable, "database response timeout"
        line = self.proc.stdout.readline()
        assert line, "database closed stdout"
        return json.loads(line)

    def request(self, sql, mode="exact"):
        self.ident += 1
        request = {"id": self.ident, "op": "execute", "sql": sql, "search_mode": mode}
        self.proc.stdin.write(json.dumps(request) + "\n")
        self.proc.stdin.flush()
        response = self.read()
        assert response.get("id") == self.ident, response
        assert response.get("ok") is True, (sql[:100], response)
        return response

    def close(self):
        self.proc.stdin.close()
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        finally:
            self.proc.stdout.close()
            self.proc.stderr.close()


def check_bounded_query(binary, directory, expected):
    session = Session(binary, directory)
    try:
        status = Path(f"/proc/{session.proc.pid}/status").read_text()
        vm_kib = int(next(line.split()[1] for line in status.splitlines()
                          if line.startswith("VmSize:")))
        # The mapped table and PK directory are already open. A query returning
        # zero or one row needs far less than another MiB; 150k heap entries do not.
        _, hard = resource.prlimit(session.proc.pid, resource.RLIMIT_AS)
        soft = vm_kib * 1024 + 1024 * 1024
        if hard != resource.RLIM_INFINITY:
            soft = min(soft, hard)
        resource.prlimit(session.proc.pid, resource.RLIMIT_AS, (soft, hard))
        for mode in ("exact", "ann"):
            result = session.request(
                "SELECT id FROM churn ORDER BY L2_DISTANCE(v, '[0]') "
                "LIMIT 9223372036854775807;", mode)
            assert result["columns"] == ["id"] and result["rows"] == expected, result
    finally:
        session.close()


def run(binary):
    count = 150000
    with tempfile.TemporaryDirectory(prefix="mvd-topk-memory-") as directory:
        session = Session(binary, directory)
        try:
            session.request("CREATE TABLE churn (id BIGINT PRIMARY KEY, v VECTOR(1));")
            for start in range(1, count + 1, 500):
                values = ",".join(f"({ident},'[3]')" for ident in range(start, start + 500))
                response = session.request("INSERT INTO churn VALUES " + values + ";")
                assert response["affected_rows"] == 500, response
            response = session.request(f"DELETE FROM churn WHERE id < {count};")
            assert response["affected_rows"] == count - 1, response
        finally:
            session.close()

        check_bounded_query(binary, directory, [[count]])
        session = Session(binary, directory)
        try:
            assert session.request("DELETE FROM churn;")["affected_rows"] == 1
        finally:
            session.close()
        check_bounded_query(binary, directory, [])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="./build/minivectordb")
    args = parser.parse_args()
    run(str(Path(args.binary).resolve()))
    print("TopK memory bound checks passed (150000 historical slots, 1/0 live rows)")
