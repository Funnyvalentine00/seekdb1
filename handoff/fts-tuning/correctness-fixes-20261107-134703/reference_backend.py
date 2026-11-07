"""Read-only source use, fresh reference instances and exclusive evidence files."""
import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import re
import select
import signal
import socket
import statistics
import subprocess
import sys
import threading
import time

BENCH = dict(ROWS="20000", BATCH="500", ROUNDS="3000", QUERY_ROUNDS="200",
             SAMPLES="3", WARMUP="30", SKIP_LOAD="0", OUTPUT="")
HITS = dict(query_cn_hits=8001, query_beng_hits=11000,
            query_mixed_hits=7332, query_limit_hits=20)
DDL = {
    "cold_ik_all": "ALTER TABLE docs ADD FULLTEXT INDEX fti_all_ik(title, content) WITH PARSER ik;",
    "warm_ik_content": "ALTER TABLE docs ADD FULLTEXT INDEX fti_content_ik(content) WITH PARSER ik;",
}
MEMORY_SQL = ("SELECT MOD_NAME,SUM(HOLD),SUM(USED) FROM oceanbase." + chr(96)
              + "V$OB_MEMORY" + chr(96) + " GROUP BY MOD_NAME ORDER BY MOD_NAME")
PS = "/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe"


def write_json(path, record):
    with path.open("x") as f:
        json.dump(record, f, indent=2, ensure_ascii=False)
        f.write("\n")


def sha(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        for part in iter(lambda: f.read(8 << 20), b""):
            h.update(part)
    return h.hexdigest()


def command(args, data=None, timeout=30):
    return subprocess.run(args, input=data, stdin=subprocess.DEVNULL if data is None else None,
                          capture_output=True, timeout=timeout)


def checked(p):
    if p.returncode:
        raise RuntimeError(f"exit {p.returncode}: {p.stderr[-2500:].decode(errors='replace')}")
    return p


def rss(pid):
    proc = Path("/proc") / str(pid)
    fields = re.findall(r"^(VmRSS|VmHWM|RssAnon|RssFile|RssShmem):\s+(\d+) kB$",
                        (proc / "status").read_text(), re.M)
    stat = (proc / "stat").read_text().rsplit(")", 1)[1].split()
    return dict(t_ns=time.time_ns(), cpu_seconds=(int(stat[11]) + int(stat[12])) / os.sysconf("SC_CLK_TCK"),
                threads=len(list((proc / "task").iterdir())),
                **{k: int(v) * 1024 for k, v in fields})


def workers(pid):
    out = {}
    for task in (Path("/proc") / str(pid) / "task").iterdir():
        try:
            if (task / "comm").read_text().strip() == "ReqWorker":
                out[task.name] = int((task / "schedstat").read_text().split()[0])
        except FileNotFoundError:
            continue
    return out


def stop_group(p):
    if p.poll() is None:
        os.killpg(p.pid, signal.SIGTERM)
        try:
            p.wait(timeout=40)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait(timeout=10)


class Session:
    def __init__(self, manifest):
        self.cfg = json.loads(manifest.read_text())
        self.run = Path(self.cfg["node_root"]).resolve(strict=True)
        self.work = manifest.parent.resolve(strict=True)
        if self.work.parent != self.run or self.work.name != self.cfg["session"]:
            raise ValueError("manifest must be in its unique session directory")
        self.src = self.run / "src"
        self.bench = self.src / "tools/benchmark"
        self.ref = Path(self.cfg["reference"]["path"]).resolve(strict=True)
        if self.ref.parent != self.run / "saved-binaries":
            raise ValueError("expected sealed reference")
        self.cpus = self.cfg["server_cpus"]
        self.client_cpu = self.cfg["client_cpu"]
        available = os.sched_getaffinity(0)
        if self.client_cpu in self.cpus or not (set(self.cpus) | {self.client_cpu}) <= available:
            raise ValueError("unavailable or overlapping CPU assignment")
        self.port = self.cfg["port"]
        if self.port != 2920:
            raise ValueError("expected isolated port 2920")
        self.client = self.src / "deps/3rd/u01/obclient/bin/obclient"
        self.mysql = ["taskset", "-c", str(self.client_cpu), str(self.client), "-h127.0.0.1",
                      "-P" + str(self.port), "-uroot", "--default-character-set=utf8mb4"]
        self.sql = self.mysql + ["-N", "-s", "-B"]
        self.guard_path = self.run / "resource_guard.py"
        self.cpu_max = self.cfg["host_cpu_max_percent"]

    def preflight(self):
        if self.cfg["reference"]["sha256"] != sha(self.ref):
            raise RuntimeError("sealed reference hash changed")
        mismatches = [path for path, digest in self.cfg["files"].items()
                      if sha(self.src / path) != digest]
        if mismatches:
            raise RuntimeError("source identity mismatch: " + ", ".join(mismatches))
        self.guard()

    def guard(self):
        p = checked(command([sys.executable, str(self.guard_path)], timeout=25))
        return json.loads(p.stdout.decode().split("RESOURCE_GUARD ", 1)[1])

    def host_load(self):
        ps = "(Get-CimInstance Win32_Processor | Select-Object LoadPercentage,CurrentClockSpeed,MaxClockSpeed | ConvertTo-Json -Compress)"
        p = checked(command([PS, "-NoProfile", "-Command", ps], timeout=15))
        d = json.loads(p.stdout)
        rows = d if isinstance(d, list) else [d]
        return dict(t_ns=time.time_ns(), reported_processors=rows,
                    max_load_pct=max(float(x["LoadPercentage"]) for x in rows))

    @contextlib.contextmanager
    def instance(self, tag, out):
        resources = self.guard()
        out.mkdir()
        with socket.socket() as s:
            s.settimeout(.5)
            if s.connect_ex(("127.0.0.1", self.port)) == 0:
                raise RuntimeError("test port has an existing listener")
        data = out / "data"
        data.mkdir()
        (data / ".owned-fts-session").write_text(self.cfg["session"] + "/" + tag + "\n")
        argv = ["taskset", "-c", ",".join(map(str, self.cpus)), str(self.ref), "--nodaemon",
                "--port", str(self.port), "--base-dir", str(data),
                "--parameter", "datafile_size=2G", "--parameter", "datafile_maxsize=2G",
                "--parameter", "memory_budget=2G", "--parameter", "vector_memory_limit=512M",
                "--parameter", "log_disk_size=2G", "--parameter", "cpu_count=4"]
        with (out / "server.stdout").open("xb") as log:
            p = subprocess.Popen(argv, cwd=data, stdin=subprocess.DEVNULL,
                                 stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            done = threading.Event()
            errors = []

            def watch():
                while not done.wait(30):
                    try:
                        with (out / "resource-samples.jsonl").open("a") as f:
                            f.write(json.dumps(self.guard()) + "\n")
                    except Exception as exc:
                        errors.append(repr(exc))
                        if p.poll() is None:
                            p.terminate()
                        return

            thread = threading.Thread(target=watch)
            thread.start()
            try:
                for _ in range(120):
                    if p.poll() is not None:
                        raise RuntimeError(f"server exited: {p.returncode}")
                    listeners = checked(command(["ss", "-ltnpH", f"sport = :{self.port}"], timeout=5)).stdout
                    if listeners:
                        if any(f"pid={p.pid},".encode() not in line for line in listeners.splitlines()):
                            raise RuntimeError("test listener is not owned by this instance")
                        q = command(self.sql + ["-e", "SELECT 1"], timeout=5)
                        if q.returncode == 0:
                            if Path(f"/proc/{p.pid}/exe").resolve() != self.ref:
                                raise RuntimeError("listener readiness does not identify own reference")
                            break
                    time.sleep(.5)
                else:
                    raise TimeoutError("server readiness")
                write_json(out / "instance.json", dict(tag=tag, pid=p.pid, command=argv,
                           source_product_sha=self.cfg["source_product_sha"],
                           binary_sha256=self.cfg["reference"]["sha256"],
                           affinity=sorted(os.sched_getaffinity(p.pid)), resources=resources))
                print("INSTANCE", tag, p.pid, flush=True)
                yield p, data
                if errors:
                    raise RuntimeError("resource monitor: " + "; ".join(errors))
            finally:
                done.set()
                stop_group(p)
                thread.join()
                write_json(out / "stopped.json", dict(pid=p.pid, returncode=p.returncode,
                           data_preserved=str(data), resource_errors=errors))

    def benchmark(self, tag, out, p):
        env = dict(os.environ, **BENCH, LABEL=tag, MYSQL=" ".join(self.sql),
                   MYSQL_VERBOSE=" ".join(self.mysql))
        before = rss(p.pid)
        start = time.time_ns()
        with (out / "official.txt").open("xb") as stdout, (out / "official.stderr").open("xb") as stderr:
            b = subprocess.run(["bash", str(self.bench / "fts_large_bench.sh")], env=env,
                               stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr, timeout=600)
        after = rss(p.pid)
        if b.returncode:
            raise RuntimeError(f"official benchmark exit {b.returncode}, see {out}/official.stderr")
        metrics = {k: float(v) for k, v in re.findall(r"^(\w+):\s*([\d.]+)\s*$",
                   (out / "official.txt").read_text(), re.M)}
        for k, v in HITS.items():
            if metrics.get(k) != v:
                raise RuntimeError(f"{k}: expected {v}, got {metrics.get(k)}")
        scored = checked(command([sys.executable, str(self.bench / "fts_large_bench_score.py"),
                                  str(out / "official.txt"), "--json"]))
        with (out / "score.json").open("xb") as f:
            f.write(scored.stdout)
        return dict(bench_begin_ns=start, bench_end_ns=after["t_ns"], metrics=metrics,
                    rss_before=before, rss_benchmark=after, benchmark_parameters=BENCH)

    def aa(self, window):
        # Preserve every attempted run. Decisions use the two predeclared consecutive pairs.
        for index, suffix in enumerate(["aa1a", "aa1b", "aa2b", "aa2a"], 1):
            tag = window + "-" + suffix
            begin = self.host_load()
            out = self.work / tag
            if begin["max_load_pct"] > self.cpu_max:
                write_json(self.work / f"aa-stopped-before-{tag}.json", begin)
                print("HOST_LOAD_ABOVE_LIMIT", tag, begin["max_load_pct"], flush=True)
                return
            with self.instance(tag, out) as (p, _):
                rec = self.benchmark(tag, out, p)
                rec["host_before"] = begin
                rec["host_after"] = self.host_load()
                write_json(out / "official-summary.json", rec)
                write_json(out / "tails.json", self.tails(p.pid))
                print("AA_COMPLETE", index, tag, rec["metrics"]["select1_avg_ms"],
                      rec["rss_benchmark"]["VmHWM"], flush=True)
            if rec["host_after"]["max_load_pct"] > self.cpu_max:
                print("HOST_LOAD_ABOVE_LIMIT_AFTER", tag, rec["host_after"]["max_load_pct"], flush=True)
                return

    def tails(self, pid):
        workload = json.loads((self.work / "workloads.json").read_text())
        out = {}
        for name, sql in workload.items():
            p = subprocess.Popen(self.sql + ["--raw", "--unbuffered"], stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 start_new_session=True)
            samples = []

            def execute(q):
                p.stdin.write((q + "\n").encode())
                p.stdin.flush()
                if not select.select([p.stdout], [], [], 15)[0]:
                    raise TimeoutError(name)
                if not p.stdout.readline():
                    raise RuntimeError(f"unexpected EOF on {name}")

            try:
                execute("SET NAMES utf8mb4; SELECT 1;")
                for _ in range(30):
                    execute(sql)
                before = workers(pid)
                for _ in range(1000):
                    start = time.perf_counter_ns()
                    execute(sql)
                    samples.append((time.perf_counter_ns() - start) / 1e6)
                after = workers(pid)
                p.stdin.close()
                if p.wait(timeout=6) != 0:
                    raise RuntimeError(f"client exit {p.returncode} on {name}")
            finally:
                stop_group(p)
                p.stdout.close()
                p.stderr.close()
                if not p.stdin.closed:
                    p.stdin.close()
            quantiles = statistics.quantiles(samples, n=100, method="inclusive")
            out[name] = dict(latencies_ms=samples, n=len(samples), p95_ms=quantiles[94],
                             p99_ms=quantiles[98], workers_before=before, workers_after=after,
                             worker_cpu_ns_per_query=((sum(after.values()) - sum(before.values())) / len(samples)
                                                     if before and before.keys() == after.keys() else None))
        return out

    def cold(self, tag, profile):
        out = self.work / tag
        with self.instance(tag, out) as (p, data):
            setup = (self.bench / "fts_large_bench_setup.sql").read_bytes()
            checked(command(self.mysql, setup))
            generated = checked(command([sys.executable, str(self.bench / "fts_large_bench_gen.py"),
                                         "--rows", BENCH["ROWS"], "--batch", BENCH["BATCH"]], timeout=60))
            checked(command(self.mysql, generated.stdout, timeout=120))
            nrows = checked(command(self.sql + ["-e", "SELECT COUNT(*) FROM fts_large_bench.docs"]))
            if nrows.stdout.strip() != b"20000":
                raise RuntimeError("unexpected diagnostic dataset row count")
            write_json(out / "dataset.json", dict(rows=20000, generated_sql_sha256=hashlib.sha256(generated.stdout).hexdigest()))
            self.trace_ddl(p, out, "cold_ik_all", mem=True, profile=False)
            self.trace_ddl(p, out, "warm_ik_content", mem=False, profile=profile)
            self.log_markers(data, out)

    def trace_ddl(self, p, out, phase, mem, profile):
        done = threading.Event()
        samples, labels, errors = [], [], []

        def status_samples():
            try:
                while not done.is_set():
                    samples.append(rss(p.pid))
                    done.wait(.025)
            except Exception as exc:
                errors.append(repr(exc))

        def memory_sample():
            start = time.time_ns()
            q = command(self.sql + ["-e", MEMORY_SQL], timeout=6)
            return dict(begin_ns=start, end_ns=time.time_ns(), rc=q.returncode,
                        rows=q.stdout.decode(errors="replace"), stderr=q.stderr.decode(errors="replace"))

        def label_samples():
            try:
                while not done.is_set():
                    labels.append(memory_sample())
                    if labels[-1]["rc"]:
                        return
                    done.wait(.18)
            except Exception as exc:
                errors.append(repr(exc))

        before = rss(p.pid)
        if mem:
            labels.append(memory_sample())
        thread = threading.Thread(target=status_samples)
        label_thread = threading.Thread(target=label_samples)
        perf = None
        perf_err = None
        datafile = out / "warm-profile.data"
        if profile:
            perf_err = (out / "warm-profile-record.log").open("xb")
            perf = subprocess.Popen([self.cfg["profile_perf"], "record", "-e", self.cfg["profile_cpu_event"],
                                     "-F", "97", "--call-graph", "dwarf,8192", "-p", str(p.pid),
                                     "-o", str(datafile)], stdin=subprocess.DEVNULL,
                                    stdout=perf_err, stderr=subprocess.STDOUT, start_new_session=True)
            time.sleep(.15)
        thread.start()
        if mem and labels[-1]["rc"] == 0:
            label_thread.start()
        start = time.time_ns()
        result, after, failure = None, None, None
        try:
            result = command(self.sql + ["-e", "USE fts_large_bench; " + DDL[phase]], timeout=150)
            after = rss(p.pid)
        except Exception as exc:
            failure = repr(exc)
        finally:
            done.set()
            thread.join()
            if label_thread.ident is not None:
                label_thread.join()
            if perf:
                if perf.poll() is None:
                    perf.send_signal(signal.SIGINT)
                    try:
                        perf.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        stop_group(perf)
                perf_err.close()
        if mem and failure is None:
            labels.append(memory_sample())
        write_json(out / (phase + ".json"), dict(before=before, after=after, begin_ns=start,
                   rc=None if result is None else result.returncode,
                   elapsed_seconds=None if after is None else (after["t_ns"] - start) / 1e9,
                   sql=DDL[phase], status_samples=samples, allocator_labels=labels, errors=errors,
                   failure=failure, stdout=None if result is None else result.stdout.decode(errors="replace"),
                   stderr=None if result is None else result.stderr.decode(errors="replace")))
        if failure is not None:
            raise RuntimeError(failure)
        checked(result)
        if datafile.is_file() and datafile.stat().st_size and perf.returncode == 0:
            for children, name in [("--no-children", "warm-profile-leaf.txt"),
                                   ("--children", "warm-profile-inclusive.txt")]:
                with (out / name).open("xb") as f:
                    args = [self.cfg["profile_perf"], "report", "--stdio", children,
                            "--show-nr-samples", "--percent-limit", "0", "-i", str(datafile)]
                    subprocess.run(args, stdin=subprocess.DEVNULL, stdout=f, stderr=subprocess.STDOUT, timeout=50)
        print("DDL_DIAG", phase, round((after["t_ns"] - start) / 1e9, 3),
              "peak", after["VmHWM"], "label_samples", len(labels),
              "perf_rc", None if perf is None else perf.returncode, flush=True)

    def log_markers(self, data, out):
        patterns = (b"build dat finished", b"build_ranges_concurrently_thread_pool completed",
                    b"finish wait ddl", b"parallel ddl wait schema")
        candidates = [p for p in (data / "log").glob("*.log*") if p.is_file() and not p.is_symlink()]
        with (out / "ddl-log-markers.txt").open("xb") as f:
            for file in candidates:
                if not file.name.startswith(("seekdb.log", "observer.log")):
                    continue
                with file.open("rb") as source:
                    for line in source:
                        if any(s in line for s in patterns):
                            f.write(line)


def terminate(_signum, _frame):
    raise KeyboardInterrupt("session termination")


def main():
    signal.signal(signal.SIGTERM, terminate)
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("mode", choices=["cold", "aa"])
    parser.add_argument("--tag", choices=["cold1", "cold2"], default="cold1")
    parser.add_argument("--profile", action="store_true")
    parser.add_argument("--window", choices=["w2"], default="w2")
    args = parser.parse_args()
    session = Session(args.manifest)
    session.preflight()
    if args.mode == "aa":
        session.aa(args.window)
    else:
        session.cold(args.tag, args.profile)


if __name__ == "__main__":
    main()
