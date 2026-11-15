#!/usr/bin/env python3
"""Run reproducible correctness gates, preserving all logs and binary hashes.

No performance acceptance is inferred. Linux-only memory gates are explicitly
reported as skipped on other hosts. Use --seeds 20 --rounds 500 --steps 1000
for an extended local campaign; use a sanitizer binary for a second campaign.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, help="Old binary for cross-version WAL checks; omitted means self-recovery only")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seeds", type=int, default=5)
    parser.add_argument("--rounds", type=int, default=200)
    parser.add_argument("--steps", type=int, default=500)
    parser.add_argument("--large-rows", type=int, default=200000)
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()
    if min(args.seeds, args.rounds, args.steps, args.large_rows, args.timeout) < 1:
        parser.error("counts and timeout must be positive")
    args.binary = args.binary.resolve()
    args.baseline = (args.baseline or args.binary).resolve()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    # Child stderr is captured by several existing test drivers. Force fatal
    # sanitizer diagnostics so those drivers cannot silently discard a report.
    environment = os.environ.copy()
    for name in ("ASAN_OPTIONS", "UBSAN_OPTIONS"):
        environment[name] = environment.get(name, "") + ":halt_on_error=1:abort_on_error=1"
    report = {"platform": platform.platform(), "python": sys.version,
              "binary": str(args.binary), "baseline": str(args.baseline),
              "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(),
              "baseline_sha256": hashlib.sha256(args.baseline.read_bytes()).hexdigest(),
              "parameters": {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
              "sanitizer_options": {name: environment[name] for name in ("ASAN_OPTIONS", "UBSAN_OPTIONS")},
              "checks": [], "skipped": []}
    cross_version = report["binary_sha256"] != report["baseline_sha256"]
    report["wal_mode"] = "cross-version" if cross_version else "same-binary-self-recovery"

    def save():
        (args.output / "summary.json").write_text(json.dumps(report, indent=2) + "\n")

    def run(name, script, *extra):
        command = [sys.executable, str(ROOT / "tests" / script), "--binary", str(args.binary), *map(str, extra)]
        if script in ("wal_compatibility.py", "protocol_differential.py"):
            command = [sys.executable, str(ROOT / "tests" / script), "--baseline", str(args.baseline),
                       "--candidate", str(args.binary), *map(str, extra)]
        log = args.output / (name + ".log")
        entry = {"name": name, "command": command, "log": str(log), "status": "running"}
        report["checks"].append(entry)
        save()
        started = time.monotonic()
        with log.open("w") as stream:
            stream.write(json.dumps(command) + "\n")
            stream.flush()
            proc = subprocess.Popen(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT,
                                    start_new_session=True, env=environment)
            try:
                entry["returncode"] = proc.wait(timeout=args.timeout)
                entry["status"] = "passed" if proc.returncode == 0 else "failed"
            except subprocess.TimeoutExpired:
                entry["status"] = "timeout"
            finally:
                # Kill only this validation command's own process group, including
                # orphaned database children after an assertion or timeout.
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                proc.wait()
        entry["seconds"] = round(time.monotonic() - started, 3)
        diagnostics = log.read_text(errors="replace")
        if any(marker in diagnostics for marker in ("ERROR: AddressSanitizer", "runtime error:", "ERROR: LeakSanitizer")):
            entry["status"] = "failed-sanitizer"
        save()
        print(f"{name}: {entry['status']} ({entry['seconds']}s)", flush=True)

    run("public", "run_cases.py", ROOT / "mysqltest/cases")
    run("regressions", "run_cases.py", ROOT / "tests/cases")
    run("stdio", "stdio_protocol.py")
    run("query-boundaries", "query_boundaries.py", "-v")
    run("topk-dimensions", "topk_dimensions.py")
    run("filtered-topk", "filtered_topk.py")
    run("wal-cross-version" if cross_version else "wal-self-recovery", "wal_compatibility.py")
    run("protocol-cross-version" if cross_version else "protocol-self-check", "protocol_differential.py",
        "--seed", 42, "--requests", 3000)
    run("wal-checkpoint", "wal_checkpoint.py")
    for seed in range(1, args.seeds + 1):
        run(f"fuzz-{seed}", "fuzz.py", "--seed", seed, "--rounds", args.rounds)
        run(f"transaction-{seed}", "transaction_model.py", "--seed", seed, "--steps", args.steps,
            "--failure-trace", args.output / f"transaction-{seed}-failure.sql")
    run("large-recovery", "bigdata.py", "--n", args.large_rows, "--dim", 32, "--batch", 1000)
    run("large-batch-recovery", "bigdata.py", "--n", 10000, "--dim", 128, "--batch", 10000)
    if sys.platform.startswith("linux"):
        run("topk-memory", "topk_memory.py")
        run("filtered-topk-memory", "filtered_topk.py", "--memory")
    else:
        report["skipped"] += ["topk-memory: requires Linux /proc and prlimit",
                              "filtered-topk-memory: requires Linux /proc and prlimit"]
    report["passed"] = all(check["status"] == "passed" for check in report["checks"])
    save()
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
