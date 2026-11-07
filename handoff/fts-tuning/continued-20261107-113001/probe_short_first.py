"""Inspect already allocated buffers in a disposable instance of the sealed reference."""
import collections
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import threading
import time

BASE = Path(__file__).resolve().parent
WORK = BASE / "short-first"
NODE = BASE.parent
REF = NODE / "saved-binaries/seekdb-fixed"
EXPECTED_REF = "9544ca38c4d6cc336adb079f362eff1fa220739d58ce70bc2d0f1d948d5ade32"
PORT = 2920
CLIENT = NODE / "src/deps/3rd/u01/obclient/bin/obclient"
CASES = ["prepared_short_then_1024"]


def sha(p):
    h = hashlib.sha256()
    with p.open("rb") as f:
        for b in iter(lambda: f.read(8 << 20), b""):
            h.update(b)
    return h.hexdigest()


def save(name, value):
    with (WORK / name).open("x") as f:
        json.dump(value, f, ensure_ascii=False, indent=2)
        f.write("\n")


def cmd(args, timeout=30):
    return subprocess.run(args, stdin=subprocess.DEVNULL, capture_output=True, timeout=timeout)


def guard():
    p = cmd([sys.executable, str(NODE / "resource_guard.py")], 25)
    p.check_returncode()
    return json.loads(p.stdout.decode().split("RESOURCE_GUARD ", 1)[1])


def sql(q, timeout=100):
    return cmd(["taskset", "-c", "4", str(CLIENT), "-h127.0.0.1", "-P" + str(PORT),
                "-uroot", "-N", "-s", "-B", "--default-character-set=utf8mb4", "-e", q], timeout)


def listeners():
    p = cmd(["ss", "-ltnpH", f"sport = :{PORT}"], 5)
    p.check_returncode()
    return p.stdout.decode()


def server_owned(pid, gdb_pid):
    proc = Path("/proc") / str(pid)
    try:
        status = (proc / "status").read_text()
        return (proc / "exe").resolve(strict=True) == REF and re.search(r"^PPid:\s+" + str(gdb_pid) + r"$", status, re.M)
    except (FileNotFoundError, ProcessLookupError, PermissionError):
        return False


def readiness(g, logpath):
    for _ in range(100):
        if g.poll() is not None:
            raise RuntimeError(f"GDB or inferior exited {g.returncode}. See {logpath}")
        lines = listeners()
        if lines:
            pids = {int(p) for p in re.findall(r"pid=(\d+),", lines)}
            if len(pids) != 1:
                raise RuntimeError("ambiguous owner of test port")
            pid = pids.pop()
            if not server_owned(pid, g.pid):
                raise RuntimeError("test port owned by an unrelated process")
            if sql("SELECT 1", 5).returncode == 0:
                return pid
        time.sleep(.3)
    raise TimeoutError("isolated reference did not start")


def grouped(records):
    counts = collections.Counter(tuple(sorted((k, v) for k, v in x.items() if k != "init_id")) for x in records)
    out = []
    for fields, count in sorted(counts.items()):
        row = dict(fields, token_inits=count)
        row["score_payload_bytes"] = count * row["score_capacity"] * row["score_item_bytes"] if row["score_allocated"] else 0
        row["docid_payload_bytes"] = count * row["doc_capacity"] * row["doc_item_bytes"] if row["doc_allocated"] else 0
        out.append(row)
    return out


def main():
    def terminate(_signum, _frame):
        raise KeyboardInterrupt("terminate allocation probe")

    signal.signal(signal.SIGTERM, terminate)
    if BASE.name != "continued-20261107-113001" or sha(REF) != EXPECTED_REF:
        raise RuntimeError("unexpected probe path or sealed reference")
    resource = guard()
    if listeners():
        raise RuntimeError("test port already in use")
    fixture = NODE / "edge-cases.json"
    corpus = json.loads(fixture.read_text())
    workdata = NODE / "data" / "p2-dbg-113001"
    previous = json.loads((BASE / "followup/stopped.json").read_text())
    if previous["gdb_rc"] != 0 or previous["stop_signal"] != "SIGUSR1" or previous["listeners"]:
        raise RuntimeError("previous probe did not shut down cleanly")
    if Path(previous["data_retained"]) != workdata or (workdata / ".owned-fts-session").read_text().strip() != BASE.name:
        raise RuntimeError("owned test data marker mismatch")
    WORK.mkdir()
    logpath = WORK / "gdb.log"
    args = ["/usr/bin/gdb", "-q", "-nx", "-batch", "-x", str(BASE / "allocation_followup.gdb"), "--args",
            str(REF), "--nodaemon", "--port", str(PORT), "--base-dir", str(workdata),
            "--parameter", "memory_budget=2G", "--parameter", "vector_memory_limit=512M",
            "--parameter", "datafile_size=2G", "--parameter", "datafile_maxsize=2G",
            "--parameter", "log_disk_size=2G", "--parameter", "cpu_count=4"]
    g = None
    pid = None
    results = {}
    stop_signal = None
    done = threading.Event()
    watch = None
    resource_errors = []

    def monitor():
        while not done.wait(30):
            try:
                guard()
            except Exception as exc:
                resource_errors.append(repr(exc))
                if pid is not None and server_owned(pid, g.pid):
                    os.kill(pid, signal.SIGUSR1)
                return

    try:
        with logpath.open("xb") as log:
            g = subprocess.Popen(["taskset", "-c", "0,1,2,3"] + args, cwd=workdata, stdout=log,
                                 stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
        pid = readiness(g, logpath)
        watch = threading.Thread(target=monitor)
        watch.start()
        save("instance.json", dict(binary=str(REF), binary_sha256=EXPECTED_REF, fixture_sha256=sha(fixture),
             gdb_pid=g.pid, pid=pid, data=str(workdata), resources=resource, command=args))
        setup = sql("SELECT COUNT(*) FROM edges.docs;")
        (WORK / "setup.stderr").write_bytes(setup.stderr)
        setup.check_returncode()
        if setup.stdout.strip() != b"1000":
            raise RuntimeError("existing owned fixture has unexpected row count")
        literal = corpus["tokens_1024"][1]
        words = re.search(r"AGAINST\('([^']+)'\)", literal)[1]
        if len(words.split()) != 1024 or not words.startswith("database "):
            raise RuntimeError("unexpected 1024 term corpus")
        with logpath.open() as log:
            log.seek(0, os.SEEK_END)
            for tag in CASES:
                if resource_errors:
                    raise RuntimeError(resource_errors)
                q = ("USE edges; PREPARE p FROM 'SELECT COUNT(*) FROM docs WHERE MATCH(a,b) AGAINST(?)';"
                     "SET @q='database'; EXECUTE p USING @q;"
                     f"SET @q='{words}'; EXECUTE p USING @q;"
                     "SET @q='database'; EXECUTE p USING @q; DEALLOCATE PREPARE p;")
                p = sql(q)
                (WORK / (tag + ".out")).write_bytes(p.stdout)
                (WORK / (tag + ".err")).write_bytes(p.stderr)
                raw = log.read().splitlines()
                vals = [json.loads(line[len("FTS_ARRAY "):]) for line in raw if line.startswith("FTS_ARRAY ")]
                errors = [line for line in raw if line.startswith("FTS_ARRAY_ERROR ")]
                writes = [json.loads(l[len("FTS_DOC_LOAD "):]) for l in raw if l.startswith("FTS_DOC_LOAD ")]
                read_errors = [l for l in raw if l.startswith("FTS_DOC_ERROR ")]
                inits = {x["init_id"] for x in vals}
                written = {x["init_id"] for x in writes}
                results[tag] = dict(rc=p.returncode, groups=grouped(vals), probe_errors=errors,
                                    doc_load_errors=read_errors, observed_doc_buffers_written=len(written),
                                    observed_doc_load_calls=len(writes),
                                    doc_load_ids_in_case=written <= inits,
                                    stdout_sha256=hashlib.sha256(p.stdout).hexdigest(),
                                    stderr_sha256=hashlib.sha256(p.stderr).hexdigest())
                save(tag + ".json", results[tag])
                print(tag, p.returncode, len(vals), errors[:1], flush=True)
        save("allocation.json", results)
    finally:
        done.set()
        if watch is not None:
            watch.join()
        if g is not None:
            if pid is None and g.poll() is None:
                children = Path(f"/proc/{g.pid}/task/{g.pid}/children")
                if children.is_file():
                    pid = next((int(x) for x in children.read_text().split() if server_owned(int(x), g.pid)), None)
            if pid is not None and server_owned(pid, g.pid):
                os.kill(pid, signal.SIGUSR1)
                stop_signal = "SIGUSR1"
            try:
                g.wait(timeout=15)
            except subprocess.TimeoutExpired:
                if pid is not None and server_owned(pid, g.pid):
                    os.kill(pid, signal.SIGTERM)
                    stop_signal = "SIGTERM after timeout"
                try:
                    g.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    os.killpg(g.pid, signal.SIGKILL)
                    g.wait(timeout=6)
                    stop_signal = "own GDB process group SIGKILL after timeout"
            save("stopped.json", dict(gdb_rc=g.returncode, stop_signal=stop_signal, listeners=listeners(),
                                      data_retained=str(workdata), results=len(results), resource_errors=resource_errors))


if __name__ == "__main__":
    main()
