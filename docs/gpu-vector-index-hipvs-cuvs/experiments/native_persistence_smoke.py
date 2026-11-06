#!/usr/bin/env python3
"""Native equivalent of the Docker-only DDL persistence smoke, task-owned paths.

Reuses the stopped SQL smoke's main runtime without deleting or recreating data.
This is functional acceptance, not a comparative performance experiment.
"""
import argparse
import json
import os
import re
from pathlib import Path
import socket
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument("--root", type=Path, required=True)
parser.add_argument("--fused-coverage", action="store_true",
                    help="Use base tile128 to exercise fusion on the10K SQL fixture; no performance claim")
parser.add_argument("--run-suffix", default="")
args = parser.parse_args()
assert re.fullmatch(r"[a-z0-9_]*", args.run_suffix)
root = args.root.resolve()
base = root / "sql-acceptance-runtime/main"
out = root / (("ddl-native-fused-acceptance" if args.fused_coverage else "ddl-native-acceptance") + args.run_suffix)
out.mkdir(exist_ok=False)
observer = root / "seekdb-f97bc2823/build_release/src/observer/seekdb"
bridge_dir = root / "acceptance/runtime"
trace = out / "trace.log"
port = 29561
assert base.is_dir() and base.resolve().is_relative_to(root)
with socket.socket() as sock:
    sock.bind(("127.0.0.1", port))
env = dict(os.environ, LD_LIBRARY_PATH=f"{bridge_dir}:/opt/rocm/lib",
           OB_VSAG_TRACE="1", OB_VSAG_TRACE_FILE=str(trace), SEEKDB_CUVS_TIME="1",
           OPENBLAS_NUM_THREADS="1", OMP_NUM_THREADS="8")
env.pop("OB_VSAG_USE_CUVS", None)
env.pop("SEEKDB_CUVS_GEMM_FUSED_NORM", None)
if args.fused_coverage:
    env["SEEKDB_CUVS_GEMM_TILE_B"] = "128"
process = None
cases = []


def sql(text):
    if args.fused_coverage:
        text = text.replace("amd_e02_accept", "amd_e02_accept_fused" + args.run_suffix)
    result = subprocess.run(["mysql", "--connect-timeout=5", "--protocol=TCP",
                             "-h127.0.0.1", f"-P{port}", "-uroot", "-ANB"],
                            input="set ob_query_timeout=300000000; " + text,
                            capture_output=True, text=True, timeout=310)
    assert result.returncode == 0, result.stderr
    return result.stdout.strip()


def start(stage):
    global process
    command = [str(observer), "--nodaemon", "--port", str(port), "--data-dir", str(base / "store"),
               "--base-dir", str(base), "--parameter", "__min_full_resource_pool_memory=2147483648",
               "--parameter", "memory_budget=2G", "--parameter", "datafile_size=20G",
               "--parameter", "cpu_count=24", "--parameter", "local_ip=127.0.0.1"]
    (out / f"{stage}.command.json").write_text(json.dumps(command))
    with (out / f"{stage}.log").open("w") as log:
        process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
    for _ in range(90):
        assert process.poll() is None, f"observer exited: {process.returncode}"
        try:
            if sql("select 1") == "1":
                return
        except AssertionError:
            pass
        time.sleep(1)
    raise RuntimeError("isolated observer startup timeout")


def stop():
    global process
    if process is not None and process.poll() is None:
        # Popen owns this exact process; never stop by name or an external PID.
        process.terminate()
        try:
            process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=10)
    process = None


def ids(table, approximate=True):
    mode = "approximate" if approximate else ""
    return sql(f"select id from amd_e02_accept.{table} order by l2_distance(v,{probe}) {mode} limit 10")


def mark(name, detail):
    cases.append(dict(name=name, status="PASS", detail=detail))
    (out / "cases.json").write_text(json.dumps(cases, indent=2))


log_path = base / "log/seekdb.log"
log_offset = log_path.stat().st_size
try:
    start("first-start")
    sql("create database amd_e02_accept; use amd_e02_accept; create table items(id bigint primary key,v vector(128))")
    inserts = (root / "sql-acceptance-evidence/data/batch-base.sql").read_text()
    sql(inserts.replace("use hipvs_gate;", "use amd_e02_accept;").replace("batch_base", "items"))
    assert sql("select count(*) from amd_e02_accept.items") == "10000"
    probe = (root / "sql-acceptance-evidence/data/query0.txt").read_text().strip()
    sql("use amd_e02_accept; create vector index idx on items(v) with (distance=l2,type=hnsw,lib=cuvs,m=16,ef_construction=200,ef_search=64)")
    first, exact = ids("items"), ids("items", False)
    assert len(first.splitlines()) == 10 and first == exact
    assert "cuvs_serve" in trace.read_text()
    (out / "first.ids").write_text(first)
    (out / "exact.ids").write_text(exact)
    maps = Path(f"/proc/{process.pid}/maps").read_text()
    loaded = "\n".join(line for line in maps.splitlines() if "libseekdb_cuvs_bridge" in line)
    assert "libseekdb_cuvs_bridge-acceptance.so" in loaded, loaded
    (out / "bridge-maps.txt").write_text(loaded)
    mark("preexisting_rows_gpu_ddl", "10000 rows; exact top10; actual final bridge loaded")
    if args.fused_coverage:
        # SeekDB redirects bridge stderr into its application log after startup.
        with log_path.open("rb") as log:
            log.seek(log_offset)
            trace_text = log.read().decode(errors="replace")
        (out / "fused-counters.log").write_text("\n".join(
            line for line in trace_text.splitlines() if "fused_tiles=" in line))
        assert any(int(v) > 0 for v in re.findall(r"fused_tiles=(\d+)", trace_text)), "SQL did not execute fused kernel"
        mark("sql_fused_execution", "positive production fused tile counter; base tile128 for coverage")
    sql("use amd_e02_accept; create table cpu_items(id bigint primary key,v vector(128)); insert into cpu_items select * from items; create vector index idx_cpu on cpu_items(v) with (distance=l2,type=hnsw,lib=vsag,m=16,ef_construction=200,ef_search=200)")
    stop()
    restart_offset = trace.stat().st_size
    start("restart")
    restarted = ids("items")
    segment = trace.read_text()[restart_offset:]
    (out / "restart.trace").write_text(segment)
    assert restarted == exact and "cuvs_serve" in segment and "cuvs_build" not in segment
    mark("restart_gpu_snapshot", "exact top10; GPU served; no rebuild")
    cpu_offset = trace.stat().st_size
    assert ids("cpu_items") == exact
    segment = trace.read_text()[cpu_offset:]
    (out / "cpu-restart.trace").write_text(segment)
    assert "fdeserialize" in segment and "cuvs_" not in segment
    mark("restart_cpu_snapshot", "VSAG deserialized; exact top10; no GPU marker")
    lifecycle = log_path.read_bytes()[log_offset:].decode(errors="replace")
    (out / "lifecycle.log").write_text(lifecycle)
    assert "serialized GPU CAGRA snapshot" in lifecycle and "built=true" in lifecycle
    assert "deserialized GPU CAGRA snapshot" in lifecycle and "graph_bytes=" in lifecycle
    assert "lazily built VSAG fallback" not in lifecycle
    mark("snapshot_lifecycle", "GPU snapshot serialized/reloaded; no eager CPU fallback")
    sql("delete from amd_e02_accept.items where id=0")
    deleted = ids("items")
    assert deleted == ids("items", False) and "0" not in deleted.splitlines()
    sql(f"insert into amd_e02_accept.items values(900000000000,{probe})")
    inserted = ids("items")
    assert inserted == ids("items", False) and "900000000000" in inserted.splitlines()
    mark("post_restart_mutation_fallback", "delete/insert exact; no stale IDs")
finally:
    stop()
mark("cleanup", "owned observer stopped; data preserved")
(out / "summary.json").write_text(json.dumps(dict(status="PASS", cases=cases), indent=2))
print(json.dumps(dict(status="PASS", cases=len(cases))), flush=True)
