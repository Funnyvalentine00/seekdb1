#!/usr/bin/env python3
"""Serial four-variant JSONL timing with interleaved same-binary A/A controls.

Each ABBA/BAAB block is one observation. All samples are retained; reported
intervals are descriptive block bootstraps, not official performance scores.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import random
import select
import signal
import statistics
import subprocess
import tempfile
import time

PROFILES = {
    "default": (10000, 128, 100, 1000, "topk"),
    "batch1": (10000, 128, 1, 300, "topk"),
    "batch10000": (10000, 128, 10000, 1000, "topk"),
    "dim8": (10000, 8, 100, 1000, "topk"),
    "responses": (1000, 8, 100, 5000, "point"),
    "large-response": (1000, 128, 100, 300, "large"),
    "smoke": (1000, 8, 100, 10, "topk"),
}
METRICS = ("write_ms", "query_ms", "filtered_ms", "unfiltered_ms", "large_ms", "first_query_ms",
           "startup_ms", "recovery_warm_ms", "process_cpu_ms", "peak_rss_kib")


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def message(ident, sql):
    return (json.dumps({"id": ident, "op": "execute", "sql": sql,
                        "search_mode": "exact"}, separators=(",", ":")) + "\n").encode()


def workload(profile):
    n, dim, batch, queries, mode = PROFILES[profile]
    rng = random.Random(42)
    rows = []
    for ident in range(1, n + 1):
        vector = ",".join(str(round(rng.uniform(-1, 1), 6)) for _ in range(dim))
        rows.append(f"({ident},'c{ident % 100}','[{vector}]')")
    inserts = []
    for start in range(0, n, batch):
        chunk = rows[start:start + batch]
        ident = len(inserts) + 2
        inserts.append((message(ident, "INSERT INTO bench VALUES " + ",".join(chunk) + ";"), ident, len(chunk)))
    rng = random.Random(7)
    selects = []
    for index in range(queries):
        ident = len(inserts) + index + 2
        kind = "unfiltered"
        if mode == "point":
            sql = f"SELECT id,cat,v FROM bench WHERE id = {index % n + 1};"
        elif mode == "large":
            if index % 3 == 0:
                sql = "SELECT v FROM bench ORDER BY id LIMIT 128;"
                kind = "large"
            else:
                sql = f"SELECT id,cat FROM bench WHERE id = {index % n + 1};"
        else:
            vector = ",".join(str(round(rng.uniform(-1, 1), 6)) for _ in range(dim))
            where = ""
            if index % 2 == 0:
                where = f" WHERE cat = 'c{rng.randrange(100)}'"
                kind = "filtered"
            sql = f"SELECT id,L2_DISTANCE(v,'[{vector}]') AS d FROM bench{where} ORDER BY d LIMIT 10;"
        selects.append((message(ident, sql), ident, kind))
    digest = hashlib.sha256()
    for request, _, _ in inserts + selects:
        digest.update(request)
    return inserts, selects, digest.hexdigest()


class Session:
    def __init__(self, binary, directory, log, cpu, timeout):
        self.log = open(log, "w+b")
        self.proc = subprocess.Popen(
            [binary, "--stdio", "--data-dir=" + str(directory)], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=self.log, bufsize=0,
            preexec_fn=(lambda: os.sched_setaffinity(0, {cpu})) if cpu is not None else None)
        self.end = time.monotonic() + timeout
        self.pending = b""
        os.set_blocking(self.proc.stdin.fileno(), False)
        os.set_blocking(self.proc.stdout.fileno(), False)
        self.usage = None

    def remaining(self, deadline):
        remaining = min(deadline, self.end) - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("JSONL run/request deadline exceeded")
        return remaining

    def read(self, deadline):
        data = bytearray(self.pending)
        offset = 0
        while True:
            newline = data.find(b"\n", offset)
            if newline != -1:
                self.pending = bytes(data[newline + 1:])
                return bytes(data[:newline + 1])
            offset = len(data)
            if not select.select([self.proc.stdout], [], [], self.remaining(deadline))[0]:
                raise TimeoutError("JSONL read timeout")
            block = os.read(self.proc.stdout.fileno(), 65536)
            if not block:
                raise RuntimeError("database closed stdout")
            data.extend(block)

    def exchange(self, request):
        deadline = time.monotonic() + 30
        pending = memoryview(request)
        while pending:
            if not select.select([], [self.proc.stdin], [], self.remaining(deadline))[1]:
                raise TimeoutError("JSONL write timeout")
            pending = pending[os.write(self.proc.stdin.fileno(), pending):]
        return self.read(deadline)

    def close(self, success):
        if self.proc.returncode is not None:
            return self.usage
        try:
            if success:
                assert json.loads(self.exchange(b'{"id":"stop","op":"shutdown"}\n')) == {"id": "stop", "ok": True}
            else:
                self.kill()
            self.proc.stdin.close()
            # wait4 is the sole reaper: Popen.poll/wait would discard child rusage.
            deadline = time.monotonic() + 10
            while True:
                pid, status, usage = os.wait4(self.proc.pid, os.WNOHANG)
                if pid:
                    self.proc.returncode = os.waitstatus_to_exitcode(status)
                    self.usage = usage
                    break
                if time.monotonic() > deadline:
                    self.kill()
                    pid, status, usage = os.wait4(self.proc.pid, 0)
                    self.proc.returncode = os.waitstatus_to_exitcode(status)
                    self.usage = usage
                    raise TimeoutError("shutdown timeout")
                time.sleep(0.001)
            if success:
                assert self.proc.returncode == 0, self.proc.returncode
                self.log.seek(0)
                assert not self.log.read(), "unexpected database stderr"
            return self.usage
        finally:
            if self.proc.returncode is None:
                self.kill()
                _, status, self.usage = os.wait4(self.proc.pid, 0)
                self.proc.returncode = os.waitstatus_to_exitcode(status)
            self.proc.stdin.close()
            self.proc.stdout.close()
            self.log.close()

    def kill(self):
        # Popen.kill() polls/reaps internally on POSIX; leave wait4 as sole reaper.
        try:
            os.kill(self.proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass


def run_once(binary, profile, inputs, output, run_id, cpu, timeout, data_root):
    inserts, selects, _ = inputs
    n, dim, _, _, mode = PROFILES[profile]
    with tempfile.TemporaryDirectory(prefix="mvd-paired-", dir=data_root) as directory:
        start = time.perf_counter_ns()
        session = Session(binary, directory, output / "stderr" / f"{run_id}.log", cpu, timeout)
        succeeded = False
        result = {}
        try:
            assert json.loads(session.read(time.monotonic() + 30)) == {"type": "ready"}
            result["startup_ms"] = (time.perf_counter_ns() - start) / 1e6
            ddl = message(1, f"CREATE TABLE bench (id BIGINT PRIMARY KEY,cat VARCHAR(16),v VECTOR({dim}));")
            assert json.loads(session.exchange(ddl)) == {"id": 1, "ok": True}
            start = time.perf_counter_ns()
            replies = [session.exchange(request) for request, _, _ in inserts]
            result["write_ms"] = (time.perf_counter_ns() - start) / 1e6
            for reply, (_, ident, count) in zip(replies, inserts):
                assert json.loads(reply) == {"id": ident, "ok": True, "affected_rows": count}, reply
            elapsed = {"filtered": 0, "unfiltered": 0, "large": 0}
            replies = []
            start = time.perf_counter_ns()
            for index, (request, _, kind) in enumerate(selects):
                before = time.perf_counter_ns()
                replies.append(session.exchange(request))
                duration = time.perf_counter_ns() - before
                elapsed[kind] += duration
                if index == 0:
                    result["first_query_ms"] = duration / 1e6
            result["query_ms"] = (time.perf_counter_ns() - start) / 1e6
            result.update({kind + "_ms": value / 1e6 for kind, value in elapsed.items()})
            digest = hashlib.sha256()
            for index, (reply, (_, ident, kind)) in enumerate(zip(replies, selects)):
                response = json.loads(reply)
                assert response.get("id") == ident and response.get("ok") is True, response
                assert "rows" in response and "columns" in response, response
                if mode == "topk":
                    assert response["columns"] == ["id", "d"] and len(response["rows"]) == 10
                if kind == "large":
                    assert len(reply) > 65536 and len(response["rows"]) == 128 and response["columns"] == ["v"]
                elif mode in ("point", "large"):
                    expected_id = index % n + 1
                    assert response["columns"] == (["id", "cat", "v"] if mode == "point" else ["id", "cat"])
                    assert len(response["rows"]) == 1 and response["rows"][0][:2] == [expected_id, f"c{expected_id % 100}"]
                digest.update(reply)
            result["query_digest"] = digest.hexdigest()
            result["response_bytes"] = sum(map(len, replies))
            if platform.system() == "Linux":
                status = Path(f"/proc/{session.proc.pid}/status").read_text()
                result["database_peak_rss_kib"] = int(next(line.split()[1] for line in status.splitlines() if line.startswith("VmHWM:")))
            succeeded = True
        finally:
            usage = session.close(succeeded)
        result.update(process_cpu_ms=(usage.ru_utime + usage.ru_stime) * 1000,
                      user_cpu_ms=usage.ru_utime * 1000, system_cpu_ms=usage.ru_stime * 1000,
                      peak_rss_kib=usage.ru_maxrss / (1024 if platform.system() == "Darwin" else 1),
                      minor_faults=usage.ru_minflt, major_faults=usage.ru_majflt,
                      voluntary_switches=usage.ru_nvcsw, involuntary_switches=usage.ru_nivcsw)
        result["process_lifetime_peak_rss_kib"] = result["peak_rss_kib"]
        if "database_peak_rss_kib" in result:
            # wait4 can retain forked Python memory from before exec on Linux.
            result["peak_rss_kib"] = result["database_peak_rss_kib"]
        wal = Path(directory) / "t_1.wal"
        result["wal_sha256"] = sha(wal)
        result["wal_bytes"] = wal.stat().st_size
        assert 0 < result["wal_bytes"] < (8 << 20), "expected nonempty WAL below startup checkpoint"
        # Own just-written fixture and page cache are warm; no cold-disk claim.
        start = time.perf_counter_ns()
        reopened = Session(binary, directory, output / "stderr" / f"{run_id}-reopen.log", cpu, timeout)
        succeeded = False
        try:
            assert json.loads(reopened.read(time.monotonic() + 30)) == {"type": "ready"}
            result["recovery_warm_ms"] = (time.perf_counter_ns() - start) / 1e6
            response = json.loads(reopened.exchange(message(1, "SELECT id FROM bench ORDER BY id;")))
            assert response == {"id": 1, "ok": True, "columns": ["id"], "rows": [[i] for i in range(1, n + 1)]}
            succeeded = True
        finally:
            reopened.close(succeeded)
        assert sha(wal) == result["wal_sha256"], "warm replay changed validated WAL"
        return result


def quantile(values, fraction):
    ordered = sorted(values)
    pos = (len(ordered) - 1) * fraction
    lo = int(pos)
    return ordered[lo] + (ordered[min(lo + 1, len(ordered) - 1)] - ordered[lo]) * (pos - lo)


def summarize(records, seed):
    groups = {}
    for record in records:
        key = (record["profile"], record["comparison"], record["block"])
        groups.setdefault(key, []).append(record)
    values = {}
    for (profile, comparison, block), rows in groups.items():
        assert len(rows) == 4 and sum(r["arm"] == "A" for r in rows) == 2
        for metric in METRICS:
            if any(row[metric] <= 0 for row in rows):
                continue
            a = [math.log(row[metric]) for row in rows if row["arm"] == "A"]
            b = [math.log(row[metric]) for row in rows if row["arm"] == "B"]
            delta = statistics.mean(b) - statistics.mean(a)
            values.setdefault(profile, {}).setdefault(comparison, {}).setdefault(metric, []).append(delta)
    rng = random.Random(seed)
    summary = {}
    for profile, comparisons in values.items():
        summary[profile] = {}
        for comparison, metrics in comparisons.items():
            result = summary[profile][comparison] = {}
            for metric, logs in metrics.items():
                boot = [statistics.median(rng.choices(logs, k=len(logs))) for _ in range(2000)]
                changes = [100 * math.expm1(value) for value in logs]
                noise = [abs(100 * math.expm1(v)) for v in comparisons.get("aa", {}).get(metric, [])]
                ci = [100 * math.expm1(quantile(boot, p)) for p in (0.025, 0.975)]
                threshold = max(3, quantile(noise, 0.95)) if noise else 3
                result[metric] = {"blocks": len(logs), "median_change_pct": 100 * math.expm1(statistics.median(logs)),
                                  "block_bootstrap_95pct": ci, "block_changes_pct": changes,
                                  "aa_p95_absolute_change_pct": quantile(noise, 0.95) if noise else None,
                                  "screen_threshold_pct": threshold,
                                  "regression_signal": comparison != "aa" and ci[0] > threshold,
                                  "base_median": statistics.median(r[metric] for r in records if r["profile"] == profile and r["comparison"] == comparison and r["arm"] == "A"),
                                  "candidate_median": statistics.median(r[metric] for r in records if r["profile"] == profile and r["comparison"] == comparison and r["arm"] == "B")}
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binaries", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--profiles", default="default,batch1,batch10000,dim8,responses,large-response")
    parser.add_argument("--blocks", type=int, default=6)
    parser.add_argument("--aa-blocks", type=int, default=10)
    parser.add_argument("--seed", type=int, default=20261115)
    parser.add_argument("--cpu", type=int)
    parser.add_argument("--client-cpu", type=int)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--data-root", type=Path)
    args = parser.parse_args()
    if not __debug__:
        parser.error("Python optimization disables validation assertions; do not use -O/PYTHONOPTIMIZE")
    if min(args.blocks, args.aa_blocks, args.timeout) <= 0:
        parser.error("blocks and timeout must be positive")
    profiles = args.profiles.split(",")
    if any(name not in PROFILES for name in profiles) or len(set(profiles)) != len(profiles):
        parser.error("unknown or duplicate profile")
    if args.cpu is not None and (not hasattr(os, "sched_getaffinity") or args.cpu not in os.sched_getaffinity(0)):
        parser.error("--cpu must be an allowed Linux CPU")
    if args.client_cpu is not None:
        if not hasattr(os, "sched_getaffinity") or args.client_cpu not in os.sched_getaffinity(0):
            parser.error("--client-cpu must be an allowed Linux CPU")
        if args.client_cpu == args.cpu:
            parser.error("client and database CPU must differ")
        os.sched_setaffinity(0, {args.client_cpu})
    binaries = {k: str(Path(v).resolve()) for k, v in json.loads(args.binaries.read_text()).items()}
    assert set(binaries) == {"baseline", "crc", "buffer", "combined"}, binaries
    binary_hashes = {k: sha(v) for k, v in binaries.items()}
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / "stderr").mkdir()
    inputs = {name: workload(name) for name in profiles}
    metadata = {"platform": platform.platform(), "binary_sha256": binary_hashes, "binaries": binaries,
                "cpu": args.cpu, "client_cpu": args.client_cpu, "data_root": str(args.data_root or tempfile.gettempdir()),
                "seed": args.seed, "blocks": args.blocks, "aa_blocks": args.aa_blocks,
                "profiles": {name: {"parameters": PROFILES[name], "requests_sha256": inputs[name][2]} for name in profiles},
                "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "recovery": "own identical-WAL fixture, warm OS cache; not cold-disk latency"}
    (args.output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    rng = random.Random(args.seed)
    schedule = []
    orders = {}
    for profile in profiles:
        for comparison in ("aa", "crc", "buffer", "combined"):
            count = args.aa_blocks if comparison == "aa" else args.blocks
            sequence = ["ABBA", "BAAB"] * (count // 2)
            if count % 2:
                sequence.append(rng.choice(["ABBA", "BAAB"]))
            rng.shuffle(sequence)
            orders[profile, comparison] = sequence
    # Round-robin all workloads and comparisons, shuffled within each round.
    for block in range(max(args.blocks, args.aa_blocks)):
        round_blocks = [(profile, comparison, block) for profile in profiles for comparison in ("aa", "crc", "buffer", "combined")
                        if block < (args.aa_blocks if comparison == "aa" else args.blocks)]
        rng.shuffle(round_blocks)
        for profile, comparison, block_id in round_blocks:
            schedule.append({"profile": profile, "comparison": comparison, "block": block_id,
                             "order": orders[profile, comparison][block_id]})
    (args.output / "schedule.json").write_text(json.dumps(schedule, indent=2) + "\n")
    records, golden = [], {}
    with (args.output / "runs.jsonl").open("w") as stream:
        for group in schedule:
            for arm in group["order"]:
                name = "baseline" if arm == "A" or group["comparison"] == "aa" else group["comparison"]
                run_id = len(records)
                try:
                    record = run_once(binaries[name], group["profile"], inputs[group["profile"]], args.output,
                                      run_id, args.cpu, args.timeout, args.data_root)
                    digests = (record["query_digest"], record["wal_sha256"])
                    assert golden.setdefault(group["profile"], digests) == digests, "response or WAL digest mismatch"
                except BaseException as error:
                    (args.output / "failure.json").write_text(json.dumps({**group, "arm": arm, "variant": name, "run_id": run_id, "error": repr(error)}, indent=2) + "\n")
                    raise
                record.update(group, arm=arm, variant=name, run_id=run_id,
                              completed_utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
                records.append(record)
                stream.write(json.dumps(record) + "\n")
                stream.flush()
            print(f"{group['profile']} {group['comparison']} block={group['block']} {group['order']} complete ({len(records)} runs)", flush=True)
    assert binary_hashes == {k: sha(v) for k, v in binaries.items()}, "binary changed during campaign"
    (args.output / "summary.json").write_text(json.dumps(summarize(records, args.seed), indent=2) + "\n")
    print(f"completed {len(records)} serial runs", flush=True)


if __name__ == "__main__":
    main()
