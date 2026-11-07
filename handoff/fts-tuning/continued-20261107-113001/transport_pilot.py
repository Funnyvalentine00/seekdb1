"""A bounded same-instance Linux transport pilot using unchanged SQL."""
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

import probe_allocation as base

OUT = base.WORK / "transport-pilot"
WORK_DATA = base.NODE / "data/p2-dbg-113001"


def save(name, data):
    with (OUT / name).open("x") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)
        f.write("\n")


def current_host():
    ps = "/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe"
    command = "(Get-CimInstance Win32_Processor | Select-Object LoadPercentage,CurrentClockSpeed | ConvertTo-Json -Compress)"
    p = base.cmd([ps, "-NoProfile", "-Command", command], 15)
    p.check_returncode()
    return json.loads(p.stdout)


def main():
    if base.sha(base.REF) != base.EXPECTED_REF or base.listeners():
        raise RuntimeError("reference or port identity changed")
    if (WORK_DATA / ".owned-fts-session").read_text().strip() != base.WORK.name:
        raise RuntimeError("wrong data owner")
    previous = json.loads((base.WORK / "short-first/stopped.json").read_text())
    if previous["gdb_rc"] != 0 or previous["stop_signal"] != "SIGUSR1" or previous["listeners"]:
        raise RuntimeError("previous owner did not shut down cleanly")
    resource = base.guard()
    OUT.mkdir()
    source = base.NODE / "next-cycle-20261107-104032/workloads.json"
    workloads = json.loads(source.read_text())
    selected = {k: workloads[k] for k in ("select1", "tokenize_beng")}
    client = ["taskset", "-c", "4", str(base.CLIENT), "-uroot", "-N", "-s", "-B",
              "--default-character-set=utf8mb4"]
    clients = {
        "tcp": client + ["--protocol=TCP", "-h127.0.0.1", "-P2920"],
        "uds": client + ["--protocol=SOCKET", "--socket=" + str(WORK_DATA / "run/sql.sock")],
    }
    args = ["taskset", "-c", "0,1,2,3", str(base.REF), "--nodaemon", "--base-dir", str(WORK_DATA),
            "--port", "2920", "--parameter", "memory_budget=2G",
            "--parameter", "vector_memory_limit=512M", "--parameter", "datafile_size=2G",
            "--parameter", "datafile_maxsize=2G", "--parameter", "log_disk_size=2G",
            "--parameter", "cpu_count=4"]
    server = None
    order = [("tcp", "uds"), ("uds", "tcp"), ("tcp", "uds"), ("uds", "tcp")]
    rows = []
    stop = None
    try:
        with (OUT / "server.stdout").open("xb") as log:
            server = subprocess.Popen(args, cwd=WORK_DATA, stdin=subprocess.DEVNULL,
                                      stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        for _ in range(100):
            if server.poll() is not None:
                raise RuntimeError("transport test server exited")
            listening = base.listeners()
            if listening:
                pids = {int(p) for p in re.findall(r"pid=(\d+),", listening)}
                if pids != {server.pid}:
                    raise RuntimeError("transport test port not owned by this process")
                if (WORK_DATA / "run/sql.sock").is_socket():
                    q = base.cmd(clients["tcp"] + ["-e", "SELECT 1"], 5)
                    if q.returncode == 0 and q.stdout.strip() == b"1":
                        break
            time.sleep(.3)
        else:
            raise TimeoutError("transport test startup")
        save("identity.json", dict(binary_sha256=base.EXPECTED_REF, command=args, pid=server.pid,
              resources=resource, transport_order=order, workloads=selected, rounds=3000,
              thresholds_pct={"select1": 2, "tokenize_beng": 1}))
        expected = {}
        for which in clients:
            for name, sql in selected.items():
                q = subprocess.run(clients[which], input=("SET NAMES utf8mb4;\n" + (sql + "\n") * 30).encode(),
                                   capture_output=True, timeout=20)
                if q.returncode or q.stderr or len(q.stdout.splitlines()) != 30:
                    raise RuntimeError("transport pilot warmup failed")
                line = q.stdout.splitlines()[0]
                if expected.setdefault(name, line) != line:
                    raise RuntimeError("transport change affected SQL output")
        host_before = current_host()
        timer = time.monotonic()
        for i, transports in enumerate(order):
            block = {}
            for which in transports:
                record = {}
                keys = list(selected) if i % 2 == 0 else list(reversed(selected))
                for name in keys:
                    script = ("SET NAMES utf8mb4;\n" + (selected[name] + "\n") * 3000).encode()
                    t = time.perf_counter_ns()
                    q = subprocess.run(clients[which], input=script, capture_output=True, timeout=40)
                    elapsed = time.perf_counter_ns() - t
                    out, err = q.stdout.splitlines(), q.stderr
                    if q.returncode or err or out != [expected[name]] * 3000:
                        (OUT / f"{i+1}-{which}-{name}.err").write_bytes(err)
                        raise RuntimeError("transport pilot SQL failure")
                    record[name] = elapsed / 3000 / 1e6
                block[which] = record
            rows.append(block)
            save(f"block-{i+1}.json", block)
            print("TRANSPORT_BLOCK", i + 1, flush=True)
            if time.monotonic() - timer > 25:
                base.guard()
                timer = time.monotonic()
        pairs = {}
        for which in clients:
            pairs[which] = {}
            for name in selected:
                vals = [r[which][name] for r in rows]
                pairs[which][name] = dict(ms=vals, pair_delta_pct=[100*(vals[i+1]/vals[i]-1) for i in (0,2)])
        ready = all(abs(x) <= limit for name, limit in [("select1", 2), ("tokenize_beng", 1)]
                    for x in pairs["uds"][name]["pair_delta_pct"])
        save("result.json", dict(pairs=pairs, qualifies_to_try_official_uds_aa=ready,
             host_before=host_before, host_after=current_host(), official_benchmark=False))
    finally:
        if server is not None:
            if server.poll() is None:
                server.send_signal(signal.SIGUSR1)
                stop = "SIGUSR1"
                try:
                    server.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    server.terminate()
                    stop = "SIGTERM fallback"
                    try:
                        server.wait(timeout=8)
                    except subprocess.TimeoutExpired:
                        server.kill()
                        server.wait(timeout=6)
                        stop = "SIGKILL fallback"
            save("stopped.json", dict(rc=server.returncode, signal=stop, pid=server.pid,
                                      listeners=base.listeners(), data_retained=str(WORK_DATA)))


if __name__ == "__main__":
    main()
