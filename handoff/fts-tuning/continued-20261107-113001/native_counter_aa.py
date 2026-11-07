"""Native counter diagnostic, separate from the Linux release protocol."""
import hashlib
import json
import os
from pathlib import Path
import runpy
import signal
import statistics
import subprocess
import time

HERE = Path(__file__).resolve().parent
BASE = Path("/Users/xiazihan/seekdb-work")
BINARY = BASE / "saved-binaries/seekdb-fts-opt-v4-fixed-20261106"
SHA = "6e3f898d7f45744e1dacfd7cd4a91da3a91d593ff4cad7823b76265cb1f5f600"
DATA = BASE / "data/fts-native-counters-113001"
MYSQL = "/Users/xiazihan/devtools/bin/mysql"
COUNTERS = Path("/Users/xiazihan/Documents/ocean base数据库大赛/fts-tuning-kit/skills/fts-tuning/scripts/instr.py")


def sha(p):
    h = hashlib.sha256()
    with p.open("rb") as f:
        for b in iter(lambda: f.read(8 << 20), b""):
            h.update(b)
    return h.hexdigest()


def save(name, value):
    with (HERE / name).open("x") as f:
        json.dump(value, f, indent=2, ensure_ascii=False)
        f.write("\n")


def main():
    if sha(BINARY) != SHA:
        raise RuntimeError("native reference hash changed")
    if os.statvfs(DATA.parent).f_bavail * os.statvfs(DATA.parent).f_frsize < 10 * 2**30:
        raise RuntimeError("insufficient native storage")
    read = runpy.run_path(str(COUNTERS))["read"]
    edge_path = BASE / "results/fts-opt-v4-20261106/linux/edge-cases.json"
    setup, literal = json.loads(edge_path.read_text())["tokens_1024"]
    words = literal.split("AGAINST('", 1)[1].split("')", 1)[0]
    if len(words.split()) != 1024:
        raise RuntimeError("unexpected long fixture")
    prefix = ("SET NAMES utf8mb4; USE edges; "
              "PREPARE p FROM 'SELECT COUNT(*) FROM docs WHERE MATCH(a,b) AGAINST(?)'; "
              "SET @q='database'; EXECUTE p USING @q; ")
    prepared = prefix + f"SET @q='{words}'; " + "EXECUTE p USING @q;" * 24 + "DEALLOCATE PREPARE p;"
    workloads = {
        "select1": (3000, "SELECT 1;\n" * 3000, [b"1"] * 3000),
        "short_match": (300, "SELECT COUNT(*) FROM edges.docs WHERE MATCH(a,b) AGAINST('database');\n" * 300, [b"1000"] * 300),
        "literal_1024": (24, (literal + "\n") * 24, [b"1000"] * 24),
        "prepared_short_first_1024": (25, prepared, [b"1000"] * 25),
    }
    DATA.mkdir(mode=0o700)
    (DATA / ".owned-fts-session").write_text(HERE.name + "\n")
    socket_path = DATA / "run/sql.sock"
    sql = [MYSQL, "--protocol=SOCKET", "--socket=" + str(socket_path), "-uroot",
           "-N", "-s", "-B", "--default-character-set=utf8mb4"]

    def query(script, timeout=120):
        return subprocess.run(sql, input=script.encode(), capture_output=True, timeout=timeout)

    args = [str(BINARY), "--nodaemon", "--base-dir", str(DATA),
            "--parameter", "mysql_port_mode=disabled", "--parameter", "memory_budget=2G",
            "--parameter", "vector_memory_limit=512M", "--parameter", "datafile_size=2G",
            "--parameter", "datafile_maxsize=2G", "--parameter", "log_disk_size=2G",
            "--parameter", "cpu_count=4"]
    server = None
    records = []
    stop = None
    try:
        with (HERE / "native-server.stdout").open("xb") as log:
            server = subprocess.Popen(args, cwd=DATA, stdin=subprocess.DEVNULL,
                                      stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        for _ in range(90):
            if server.poll() is not None:
                raise RuntimeError(f"native reference exited {server.returncode}")
            if socket_path.is_socket() and query("SELECT 1;", 6).stdout.strip() == b"1":
                break
            time.sleep(.3)
        else:
            raise TimeoutError("native SQL socket was not ready")
        save("native-identity.json", dict(pid=server.pid, command=args, binary_sha256=SHA,
             fixture_sha256=sha(edge_path), instruction_reader_sha256=sha(COUNTERS),
             socket=str(socket_path), dataset="edge-cases.json tokens_1024"))
        init = query(setup)
        (HERE / "native-setup.stderr").write_bytes(init.stderr)
        init.check_returncode()
        for name, (n, script, expected) in workloads.items():
            # Warm exact query shapes and verify all returned rows before measuring.
            q = query(script)
            if q.returncode or q.stdout.splitlines() != expected or q.stderr:
                raise RuntimeError("native workload warmup failed " + name)
        for index in range(4):
            row = {}
            order = list(workloads) if index % 2 == 0 else list(reversed(workloads))
            for name in order:
                n, script, expected = workloads[name]
                before = read(server.pid)
                started = time.perf_counter_ns()
                q = query(script)
                elapsed = time.perf_counter_ns() - started
                after = read(server.pid)
                if q.returncode or q.stdout.splitlines() != expected or q.stderr:
                    raise RuntimeError("native measured workload changed result " + name)
                if after["instructions"] <= before["instructions"]:
                    raise RuntimeError("native instruction counter did not advance")
                row[name] = dict(n=n, before=before, after=after,
                                instructions_per_stmt=(after["instructions"]-before["instructions"])/n,
                                wall_ms_per_stmt=elapsed/n/1e6,
                                stdout_sha256=hashlib.sha256(q.stdout).hexdigest())
            records.append(row)
            save(f"native-block-{index+1}.json", row)
            print("NATIVE_BLOCK", index + 1, flush=True)
        pairs = {}
        for name in workloads:
            x = [r[name]["instructions_per_stmt"] for r in records]
            w = [r[name]["wall_ms_per_stmt"] for r in records]
            pairs[name] = dict(instructions_per_stmt=x, median=statistics.median(x),
                               counter_pair_delta_pct=[100*(x[i+1]/x[i]-1) for i in (0,2)],
                               wall_pair_delta_pct=[100*(w[i+1]/w[i]-1) for i in (0,2)])
        save("native-counter-aa.json", dict(metric="process instruction counter",
             scope="macOS UDS targeted A/A, not official Linux workload",
             counter_threshold_pct=.6, pairs=pairs))
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
                        server.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        server.kill()
                        server.wait(timeout=6)
                        stop = "SIGKILL fallback"
            save("native-stopped.json", dict(pid=server.pid, rc=server.returncode, signal=stop,
                 data=str(DATA), blocks=len(records)))


if __name__ == "__main__":
    main()
