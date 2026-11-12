#!/usr/bin/env python3
"""Pair each experimental binary with baseline using the real JSONL interface."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import select
import statistics
import subprocess
import tempfile
import threading
import time


def message(ident, sql):
    return (json.dumps({"id": ident, "op": "execute", "sql": sql,
                        "search_mode": "exact"}, separators=(",", ":")) + "\n").encode()


def workload(n, dim, batch, queries):
    rng = random.Random(42)
    rows = []
    for ident in range(1, n + 1):
        vector = ",".join(repr(round(rng.uniform(-1, 1), 6)) for _ in range(dim))
        rows.append(f"({ident},'c{ident % 100}','[{vector}]')")
    inserts = []
    for start in range(0, n, batch):
        chunk = rows[start:start + batch]
        ident = len(inserts) + 2
        inserts.append((message(ident, "INSERT INTO bench VALUES " + ",".join(chunk) + ";"),
                        ident, len(chunk)))
    rng = random.Random(7)
    selects = []
    for query in range(queries):
        vector = ",".join(repr(round(rng.uniform(-1, 1), 6)) for _ in range(dim))
        filtered = query % 2 == 0
        where = f" WHERE cat = 'c{rng.randrange(100)}'" if filtered else ""
        sql = (f"SELECT id, L2_DISTANCE(v,'[{vector}]') AS d FROM bench{where} "
               "ORDER BY d LIMIT 10;")
        ident = len(inserts) + query + 2
        selects.append((message(ident, sql), ident, "filtered" if filtered else "unfiltered"))
    return inserts, selects


def read_line(proc):
    readable, _, _ = select.select([proc.stdout], [], [], 60)
    if not readable:
        raise TimeoutError("JSONL response timeout")
    line = proc.stdout.readline()
    if not line:
        raise RuntimeError("database closed stdout")
    return line


def exchange(proc, request):
    proc.stdin.write(request)
    proc.stdin.flush()
    return read_line(proc)


def cpu_ns(proc):
    return int(Path(f"/proc/{proc.pid}/schedstat").read_text().split()[0])


def run_once(binary, cpu, dim, inserts, selects, stderr_path=None, timeout=60):
    with tempfile.TemporaryDirectory(prefix="mvd-paired-") as directory:
        stderr = open(stderr_path, "w+b") if stderr_path else tempfile.TemporaryFile()
        proc = subprocess.Popen(
            [str(binary), "--stdio", "--data-dir=" + directory], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=stderr,
            preexec_fn=(lambda: os.sched_setaffinity(0, {cpu})) if cpu is not None else None)
        expired = threading.Event()

        def stop_stalled_process():
            expired.set()
            try:
                proc.kill()
            except ProcessLookupError:
                pass

        # Covers blocked writes and partial response lines, not just first-byte readiness.
        watchdog = threading.Timer(timeout, stop_stalled_process)
        watchdog.daemon = True
        watchdog.start()
        try:
            assert json.loads(read_line(proc)) == {"type": "ready"}
            ddl = message(1, f"CREATE TABLE bench (id BIGINT PRIMARY KEY, cat VARCHAR(16), v VECTOR({dim}));")
            assert json.loads(exchange(proc, ddl)) == {"id": 1, "ok": True}
            cpu_before = cpu_ns(proc)
            started = time.perf_counter_ns()
            write_replies = [exchange(proc, request) for request, _, _ in inserts]
            write_ms = (time.perf_counter_ns() - started) / 1e6
            cpu_write_ms = (cpu_ns(proc) - cpu_before) / 1e6
            # Assertions and hashing are outside the measured database request loops.
            for reply, (_, ident, count) in zip(write_replies, inserts):
                assert json.loads(reply) == {"id": ident, "ok": True, "affected_rows": count}, reply
            elapsed = {"filtered": 0, "unfiltered": 0}
            replies = []
            cpu_before = cpu_ns(proc)
            started = time.perf_counter_ns()
            for request, _, kind in selects:
                before = time.perf_counter_ns()
                reply = exchange(proc, request)
                elapsed[kind] += time.perf_counter_ns() - before
                replies.append(reply)
            query_ms = (time.perf_counter_ns() - started) / 1e6
            cpu_query_ms = (cpu_ns(proc) - cpu_before) / 1e6
            digest = hashlib.sha256()
            for reply, (_, ident, _) in zip(replies, selects):
                value = json.loads(reply)
                assert value.get("id") == ident and value.get("ok") is True, value
                assert value.get("columns") == ["id", "d"] and len(value["rows"]) == 10, value
                digest.update(json.dumps(value, sort_keys=True, separators=(",", ":")).encode())
            status = Path(f"/proc/{proc.pid}/status").read_text()
            rss = int(next(line.split()[1] for line in status.splitlines() if line.startswith("VmHWM:")))
            assert json.loads(exchange(proc, b'{"id":"stop","op":"shutdown"}\n')) == {
                "id": "stop", "ok": True}
            proc.stdin.close()
            proc.wait(timeout=30)
            assert proc.returncode == 0
            return {"write_ms": write_ms, "query_ms": query_ms,
                    "cpu_write_ms": cpu_write_ms, "cpu_query_ms": cpu_query_ms,
                    "filtered_ms": elapsed["filtered"] / 1e6,
                    "unfiltered_ms": elapsed["unfiltered"] / 1e6,
                    "peak_rss_kib": rss, "query_digest": digest.hexdigest()}
        except Exception as exc:
            if proc.poll() is None:
                proc.kill()
            proc.wait()
            stderr.seek(0)
            detail = stderr.read().decode(errors="replace")
            reason = "run deadline exceeded" if expired.is_set() else str(exc)
            raise RuntimeError(f"{reason}; stderr={stderr_path}: {detail[-8000:]}") from exc
        finally:
            watchdog.cancel()
            watchdog.join()
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            if not proc.stdin.closed:
                try:
                    proc.stdin.close()
                except BrokenPipeError:
                    pass
            proc.stdout.close()
            stderr.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binaries", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--pairs", type=int, default=5)
    parser.add_argument("--n", type=int, default=10000)
    parser.add_argument("--dim", type=int, default=128)
    parser.add_argument("--batch", type=int, default=100)
    parser.add_argument("--queries", type=int, default=1000)
    parser.add_argument("--only", help="Comma-separated candidate names; baseline is always included")
    args = parser.parse_args()
    binaries = json.loads(args.binaries.read_text())
    assert "base" in binaries and len(binaries) > 1
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "stderr").mkdir(exist_ok=True)
    cpus = sorted(os.sched_getaffinity(0))
    cpu = cpus[0]
    # Pin the client away from the server when the runner offers two CPUs.
    os.sched_setaffinity(0, {cpus[-1]})
    machine = {"uname": list(os.uname()), "cpus": cpus, "server_cpu": cpu,
               "client_cpu": cpus[-1], "lscpu": subprocess.check_output(["lscpu"], text=True),
               "workload": {"rows": args.n, "dim": args.dim, "batch": args.batch,
                            "queries": args.queries, "k": 10, "mode": "exact",
                            "filter_fraction": 0.5, "categories": 100, "pairs": args.pairs}}
    (args.output / "machine.json").write_text(json.dumps(machine, indent=2) + "\n")
    inserts, selects = workload(args.n, args.dim, args.batch, args.queries)
    records = []
    golden = None
    variants = [name for name in binaries if name != "base"]
    if args.only:
        variants = args.only.split(",")
        assert all(name != "base" and name in binaries for name in variants), variants
    with (args.output / "runs.jsonl").open("w") as log:
        for variant in variants:
            for pair in range(args.pairs):
                order = ["base", variant] if pair % 2 == 0 else [variant, "base"]
                for name in order:
                    diagnostic = args.output / "stderr" / f"{variant}-{pair}-{name}.log"
                    record = run_once(binaries[name], cpu, args.dim, inserts, selects, diagnostic)
                    if golden is None:
                        assert name == "base"
                        golden = record["query_digest"]
                    assert record["query_digest"] == golden, (variant, pair, name, record)
                    record.update({"variant": name, "comparison": variant, "pair": pair})
                    records.append(record)
                    log.write(json.dumps(record) + "\n")
                    log.flush()
                    print(json.dumps(record), flush=True)
    metrics = ["write_ms", "query_ms", "filtered_ms", "unfiltered_ms", "peak_rss_kib",
               "cpu_write_ms", "cpu_query_ms"]
    summary = {}
    for variant in variants:
        result = {}
        for metric in metrics:
            base = [r[metric] for r in records if r["comparison"] == variant and r["variant"] == "base"]
            candidate = [r[metric] for r in records if r["comparison"] == variant and r["variant"] == variant]
            result[metric] = {"base_median": statistics.median(base),
                              "candidate_median": statistics.median(candidate),
                              "paired_median_change_pct": 100 * (statistics.median(
                                  c / b for b, c in zip(base, candidate)) - 1)}
        summary[variant] = result
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2), flush=True)


if __name__ == "__main__":
    main()
