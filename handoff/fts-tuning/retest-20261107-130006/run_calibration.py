"""Bounded reference A/A; reuse one newly owned data directory, preserve results."""
import contextlib
import importlib.util
import json
import math
from pathlib import Path
import signal
import statistics
import subprocess
import sys
import threading
import time
import traceback

HERE = Path(__file__).resolve().parent
CONFIG = json.loads((HERE / "reference-manifest.json").read_text())
spec = importlib.util.spec_from_file_location("reference_backend", HERE / "reference_backend.py")
backend = importlib.util.module_from_spec(spec)
spec.loader.exec_module(backend)
if backend.sha(HERE / "reference_backend.py") != CONFIG["backend_sha256"]:
    raise RuntimeError("reference helper identity changed")


class Retest(backend.Session):
    def __init__(self):
        super().__init__(HERE / "reference-manifest.json")
        self.data = HERE / "data"
        self.normal_stop = True

    @contextlib.contextmanager
    def instance(self, tag, out):
        if not self.normal_stop:
            raise RuntimeError("previous instance did not shut down normally")
        resources = self.guard()
        out.mkdir()
        if not self.data.exists():
            self.data.mkdir()
            with (self.data / ".owned-fts-session").open("x") as f:
                f.write(self.cfg["session"] + "\n")
        if (self.data / ".owned-fts-session").read_text().strip() != self.cfg["session"]:
            raise RuntimeError("data ownership mismatch")
        ports = backend.checked(backend.command(["ss", "-ltnpH", "sport = :2920"]))
        if ports.stdout.strip():
            raise RuntimeError("test port already occupied")
        argv = ["taskset", "-c", ",".join(map(str, self.cpus)), str(self.ref), "--nodaemon",
                "--port", str(self.port), "--base-dir", str(self.data),
                "--parameter", "datafile_size=2G", "--parameter", "datafile_maxsize=2G",
                "--parameter", "memory_budget=2G", "--parameter", "vector_memory_limit=512M",
                "--parameter", "log_disk_size=2G", "--parameter", "cpu_count=4"]
        done, errors = threading.Event(), []
        with (out / "server.stdout").open("xb") as log:
            process = subprocess.Popen(argv, cwd=self.data, stdin=subprocess.DEVNULL,
                                       stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            self.normal_stop = False

            def monitor():
                while not done.wait(30):
                    try:
                        value = self.guard()
                        with (out / "resource-samples.jsonl").open("a") as f:
                            f.write(json.dumps(value) + "\n")
                    except Exception as exc:
                        errors.append(repr(exc))
                        if process.poll() is None:
                            process.send_signal(signal.SIGUSR1)
                        return

            thread = threading.Thread(target=monitor)
            thread.start()
            try:
                for _ in range(120):
                    if process.poll() is not None:
                        raise RuntimeError(f"server exited: {process.returncode}")
                    ports = backend.checked(backend.command(["ss", "-ltnpH", "sport = :2920"], timeout=5)).stdout
                    if ports:
                        if any(f"pid={process.pid},".encode() not in line for line in ports.splitlines()):
                            raise RuntimeError("listener belongs to another process")
                        q = backend.command(self.sql + ["-e", "SELECT 1"], timeout=5)
                        if q.returncode == 0 and q.stdout.strip() == b"1":
                            if Path(f"/proc/{process.pid}/exe").resolve() != self.ref:
                                raise RuntimeError("server executable identity mismatch")
                            break
                    time.sleep(.5)
                else:
                    raise TimeoutError("server readiness")
                backend.write_json(out / "instance.json", dict(pid=process.pid, command=argv,
                    tag=tag, source_product_sha=self.cfg["source_product_sha"],
                    binary_sha256=self.cfg["reference"]["sha256"], resources=resources,
                    data_reused_only_within_this_session=True))
                print("INSTANCE", tag, process.pid, flush=True)
                yield process, self.data
                if errors:
                    raise RuntimeError("resource monitor: " + repr(errors))
            finally:
                done.set()
                stop_signal = None
                if process.poll() is None:
                    process.send_signal(signal.SIGUSR1)
                    stop_signal = "SIGUSR1"
                    try:
                        process.wait(timeout=40)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        stop_signal = "SIGKILL fallback"
                        process.wait(timeout=10)
                thread.join()
                self.normal_stop = process.returncode == 0
                backend.write_json(out / "stopped.json", dict(pid=process.pid,
                    returncode=process.returncode, signal=stop_signal, normal=self.normal_stop,
                    data_retained=str(self.data), resource_errors=errors))
                if not self.normal_stop:
                    raise RuntimeError("reference shutdown abnormal; no next sample")


def power_snapshot():
    query = ("@{scheme=(powercfg.exe /getactivescheme | Out-String).Trim();"
             "power=@(Get-CimInstance -Namespace root/WMI -ClassName BatteryStatus | Select-Object PowerOnline);"
             "cpu=@(Get-CimInstance Win32_Processor | Select-Object LoadPercentage,CurrentClockSpeed,MaxClockSpeed)}"
             " | ConvertTo-Json -Depth 4 -Compress")
    result = backend.checked(backend.command([backend.PS, "-NoProfile", "-Command", query], timeout=25))
    try:
        decoded = result.stdout.decode("utf-8")
    except UnicodeDecodeError:
        decoded = result.stdout.decode("gb18030")
    return json.loads(decoded)


def analyze():
    reports = [p for p in (HERE / ("power-" + tag) for tag in ("aa1a", "aa1b", "aa2b", "aa2a"))
               if (p / "official-summary.json").exists()]
    values = [json.loads((p / "official-summary.json").read_text()) for p in reports]
    result = dict(reports=len(reports), complete_pairs=len(reports)//2,
                  new_product_experiments=0, formal_candidate_pairs=0,
                  backend_parameters=backend.BENCH, same_binary=True)
    if len(values) != 4:
        return dict(result, passed=False, reason="incomplete calibration; see progress/failure records")
    metric_keys = ["select1_avg_ms", "build_ik_all_sec", "build_ik_content_sec", "build_beng_en_sec",
                   "tokenize_ik_avg_ms", "tokenize_beng_avg_ms", "query_cn_avg_ms", "query_beng_avg_ms",
                   "query_mixed_avg_ms", "query_limit_avg_ms"]
    pairs = {}
    for key in metric_keys:
        numbers = [v["metrics"][key] for v in values]
        assert all(x > 0 and math.isfinite(x) for x in numbers)
        pairs[key] = dict(values=numbers, mean=statistics.mean(numbers), stdev=statistics.stdev(numbers),
                          pair_delta_pct=[100*(numbers[j+1]/numbers[j]-1) for j in (0, 2)])
    rss = [v["rss_benchmark"]["VmHWM"] for v in values]
    rss_pairs = [100*(rss[j+1]/rss[j]-1) for j in (0, 2)]
    failures = []
    if any(v[k]["max_load_pct"] > CONFIG["host_cpu_max_percent"]
           for v in values for k in ("host_before", "host_after")):
        failures.append("host_load")
    for key, stats in pairs.items():
        limit = 2 if key == "select1_avg_ms" else 1
        if any(abs(x) > limit for x in stats["pair_delta_pct"]):
            failures.append(key)
    if any(abs(x) > 2 for x in rss_pairs):
        failures.append("rss")
    tails = [json.loads((p / "tails.json").read_text()) for p in reports]
    tail_pairs = {}
    for key in tails[0]:
        assert all(t[key]["n"] == 1000 for t in tails)
        tail_pairs[key] = {}
        for metric, limit in (("p95_ms", 5), ("p99_ms", 10)):
            deltas = [100*(tails[j+1][key][metric]/tails[j][key][metric]-1) for j in (0, 2)]
            tail_pairs[key][metric] = deltas
            if any(abs(x) > limit for x in deltas):
                failures.append(key + ":" + metric)
    return dict(result, passed=not failures, gate_failures=failures, metrics=pairs,
                rss_bytes=rss, rss_pair_delta_pct=rss_pairs, tail_pair_delta_pct=tail_pairs,
                tail_samples=28000, authoritative_platform_score=False)


def main():
    if not CONFIG["performance_setup_user_confirmed"]:
        raise RuntimeError("performance setup confirmation absent")
    session = Retest()
    backend.write_json(HERE / "started.json", dict(t_ns=time.time_ns(), user_confirmed_setup=True,
                       no_power_settings_changed_by_driver=True))
    session.preflight()
    before = power_snapshot()
    backend.write_json(HERE / "power-before.json", before)
    if not before["power"] or not all(p["PowerOnline"] is True for p in before["power"]):
        raise RuntimeError("host AC power not confirmed")
    try:
        session.aa("power")
    finally:
        backend.write_json(HERE / "power-after.json", power_snapshot())
        backend.write_json(HERE / "resource-final.json", session.guard())
    result = analyze()
    backend.write_json(HERE / "aa-result.json", result)
    print("CALIBRATION_RESULT", json.dumps(result, ensure_ascii=False), flush=True)


def interrupted(signum, frame):
    raise InterruptedError(f"signal {signum}")


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, interrupted)
    try:
        main()
    except Exception as exc:
        backend.write_json(HERE / "failure.json", dict(error=repr(exc), traceback=traceback.format_exc()))
        raise
