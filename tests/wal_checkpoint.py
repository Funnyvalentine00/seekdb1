#!/usr/bin/env python3
"""Exercise both WAL checkpoint thresholds and SQL recovery after SIGKILL."""

import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import tempfile
import time


DIMENSION = 128
ROW_COUNT = 145_000
BATCH_SIZE = 1000
DELETE_ID = ROW_COUNT // 2
MOVED_ID = ROW_COUNT + 1
PENDING_ID = ROW_COUNT + 2
COMMIT_CHECKPOINT_BYTES = 64 << 20
STARTUP_CHECKPOINT_BYTES = 8 << 20
ZERO = [0] * DIMENSION
UPDATED = [7.5] + [0] * (DIMENSION - 2) + [-2]
ZERO_SQL = json.dumps(ZERO, separators=(",", ":"))


def expect(actual, expected, context):
    if actual != expected:
        raise AssertionError(f"{context}: expected {expected!r}, got {actual!r}")


class Session:
    def __init__(self, proc):
        self.proc = proc
        self.next_id = 0
        self.pending = b""
        os.set_blocking(proc.stdin.fileno(), False)
        os.set_blocking(proc.stdout.fileno(), False)

    def read(self, deadline):
        data = bytearray(self.pending)
        offset = 0
        while True:
            end = data.find(b"\n", offset)
            if end != -1:
                self.pending = bytes(data[end + 1:])
                return json.loads(data[:end])
            offset = len(data)
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([self.proc.stdout], [], [], remaining)[0]:
                raise TimeoutError("timed out waiting for a JSON Lines response")
            chunk = os.read(self.proc.stdout.fileno(), 65_536)
            if not chunk:
                raise RuntimeError("database closed stdout before completing a response")
            data.extend(chunk)
            if len(data) > (8 << 20):
                raise RuntimeError("database response exceeded 8 MiB")

    def request(self, op, sql=None):
        ident = self.next_id
        self.next_id += 1
        message = {"id": ident, "op": op}
        if sql is not None:
            message["sql"] = sql
        payload = memoryview((json.dumps(message) + "\n").encode())
        deadline = time.monotonic() + 30
        while payload:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([], [self.proc.stdin], [], remaining)[1]:
                raise TimeoutError("timed out sending a JSON Lines request")
            size = os.write(self.proc.stdin.fileno(), payload)
            payload = payload[size:]
        response = self.read(deadline)
        expect(response.get("id"), ident, "response id")
        expect(response.get("ok"), True, sql[:100] if sql else op)
        return response

    def execute(self, sql, affected=None):
        response = self.request("execute", sql)
        if affected is not None:
            expect(response.get("affected_rows"), affected, sql[:100])
        return response

    def kill(self):
        self.proc.kill()
        expect(self.proc.wait(timeout=10), -signal.SIGKILL, "SIGKILL exit status")

    def shutdown(self):
        self.request("shutdown")
        expect(self.proc.wait(timeout=10), 0, "shutdown exit status")


@contextmanager
def start(binary, directory):
    with tempfile.TemporaryFile() as stderr, subprocess.Popen(
            [str(binary), "--stdio", f"--data-dir={directory}"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr,
            bufsize=0) as process:
        try:
            session = Session(process)
            expect(session.read(time.monotonic() + 30), {"type": "ready"}, "startup")
            yield session
        finally:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=10)
            stderr.seek(0)
            diagnostics = stderr.read(8192).decode(errors="replace")
            if diagnostics:
                print(diagnostics)


def fill(session, wal):
    session.execute(f"CREATE TABLE points (id BIGINT PRIMARY KEY, v VECTOR({DIMENSION}));")
    previous_size = 0
    frame_bytes = None
    checkpoints = []
    for first in range(1, ROW_COUNT + 1, BATCH_SIZE):
        after_last = first + BATCH_SIZE
        values = ",".join(f"({ident},'{ZERO_SQL}')" for ident in range(first, after_last))
        session.execute("INSERT INTO points VALUES " + values + ";", BATCH_SIZE)
        size = wal.stat().st_size
        if frame_bytes is None:
            if size == 0:
                raise AssertionError("the first acknowledged batch left an empty WAL")
            frame_bytes = size
        next_size = previous_size + frame_bytes
        if size == 0:
            if not previous_size <= COMMIT_CHECKPOINT_BYTES < next_size:
                raise AssertionError(f"unexpected WAL drop from {previous_size} bytes")
            checkpoints.append((after_last - 1, previous_size, next_size))
        else:
            expect(size, next_size, "WAL size after acknowledged insert")
        previous_size = size
    expect(len(checkpoints), 1, "commit checkpoint count")
    for row_count, before, with_frame in checkpoints:
        print(f"Commit checkpoint at {row_count} rows, WAL {before} + "
              f"{with_frame - before} bytes reset to zero")
    size = wal.stat().st_size
    if size <= STARTUP_CHECKPOINT_BYTES:
        raise AssertionError(f"WAL suffix too small for startup checkpoint: {size} bytes")
    return size


def check_ids(session, mutated):
    response = session.execute("SELECT id FROM points ORDER BY id;")
    expect(response["columns"], ["id"], "sorted id columns")
    expected_ids = range(1, ROW_COUNT + 1)
    if mutated:
        expected_ids = (ident for ident in range(1, MOVED_ID + 1)
                        if ident not in (DELETE_ID, ROW_COUNT))
    rows = response["rows"]
    expect(len(rows), ROW_COUNT - int(mutated), "sorted id count")
    for index, (row, ident) in enumerate(zip(rows, expected_ids)):
        expect(row, [ident], f"sorted id at index {index}")


def check_vector(session, ident, vector):
    response = session.execute(f"SELECT id, v FROM points WHERE id = {ident};")
    expect(response["columns"], ["id", "v"], f"vector columns for {ident}")
    expect(response["rows"], [] if vector is None else [[ident, vector]], f"row {ident}")


def run(binary):
    with tempfile.TemporaryDirectory(prefix="mvd-wal-checkpoint-") as temp:
        directory = Path(temp)
        wal = directory / "t_1.wal"
        with start(binary, directory) as session:
            remaining_bytes = fill(session, wal)
            session.kill()
        expect(wal.stat().st_size, remaining_bytes, "startup suffix after SIGKILL")

        with start(binary, directory) as session:
            expect(wal.stat().st_size, 0, "WAL size after startup checkpoint")
            check_ids(session, mutated=False)
            for ident in (1, DELETE_ID, ROW_COUNT):
                check_vector(session, ident, ZERO)
            print(f"Startup checkpoint reset {remaining_bytes} bytes after SIGKILL, "
                  f"all {ROW_COUNT} IDs and sampled vectors recovered")

            session.execute("BEGIN;")
            session.execute(f"UPDATE points SET v = '{json.dumps(UPDATED)}' WHERE id = 1;", 1)
            session.execute(f"DELETE FROM points WHERE id = {DELETE_ID};", 1)
            session.execute(f"UPDATE points SET id = {MOVED_ID} WHERE id = {ROW_COUNT};", 1)
            session.execute("COMMIT;")
            committed_wal = wal.read_bytes()
            if not committed_wal:
                raise AssertionError("committed mutations did not enter the WAL")

            session.execute("BEGIN;")
            session.execute("DELETE FROM points WHERE id = 1;", 1)
            session.execute(f"UPDATE points SET v = '{json.dumps(UPDATED)}' WHERE id = 2;", 1)
            session.execute(f"INSERT INTO points VALUES ({PENDING_ID},'{ZERO_SQL}');", 1)
            check_vector(session, 1, None)
            check_vector(session, 2, UPDATED)
            check_vector(session, PENDING_ID, ZERO)
            expect(wal.read_bytes() == committed_wal, True, "uncommitted WAL change")
            session.kill()

        with start(binary, directory) as session:
            expect(wal.read_bytes() == committed_wal, True, "committed WAL after second restart")
            check_ids(session, mutated=True)
            for ident, vector in (
                    (1, UPDATED), (2, ZERO), (DELETE_ID, None), (ROW_COUNT, None),
                    (MOVED_ID, ZERO), (PENDING_ID, None)):
                check_vector(session, ident, vector)
            session.shutdown()
        print(f"Second restart recovered {ROW_COUNT - 1} IDs, committed mutations "
              "and rollback of pending writes")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("./build/minivectordb"))
    run(parser.parse_args().binary.resolve())
