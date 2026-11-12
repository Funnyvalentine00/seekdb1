#!/usr/bin/env python3
"""Build and validate eight MiniVectorDB variants on Linux; no benchmarks.

Use a fresh/empty --output directory. binaries.json contains only variants
that passed every required check; summary.json also describes failed builds.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import signal
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parent
VARIANTS = {
    "base": (), "wal": ("wal",), "filtered": ("filtered",), "scan": ("scan",),
    "wal_filtered": ("wal", "filtered"), "wal_scan": ("wal", "scan"),
    "filtered_scan": ("filtered", "scan"), "all": ("wal", "filtered", "scan"),
}
VALIDATORS = ("wal_serialization.py", "filtered_topk.py", "scan_exact_equivalence.cpp")


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def source_hashes(source):
    files = [source / "CMakeLists.txt"] + sorted(p for p in (source / "src").rglob("*") if p.is_file())
    hashes = {str(path.relative_to(source)): sha256(path) for path in files}
    digest = hashlib.sha256(json.dumps(hashes, sort_keys=True).encode()).hexdigest()
    return {"sha256": digest, "files": hashes}


class Validation:
    def __init__(self, output, summary):
        self.output, self.summary, self.binaries = output, summary, {}

    def save(self):
        for name, data in (("summary.json", self.summary), ("binaries.json", self.binaries)):
            temporary = self.output / (name + ".tmp")
            temporary.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
            temporary.replace(self.output / name)

    def run(self, variant, name, command, cwd, timeout, env=None):
        command = [str(value) for value in command]
        log = self.output / "logs" / variant / (name + ".log")
        log.parent.mkdir(parents=True, exist_ok=True)
        check = {"name": name, "command": command, "cwd": str(cwd), "log": str(log),
                 "timeout_seconds": timeout, "status": "running"}
        self.summary["variants"][variant]["checks"].append(check)
        self.save()
        print(f"[{variant}] {name} ...", flush=True)
        started = time.monotonic()
        with log.open("w") as stream:
            stream.write(f"cwd: {cwd}\ntimeout: {timeout}s\ncommand: {shlex.join(command)}\n\n")
            stream.flush()
            process = None
            try:
                process = subprocess.Popen(command, cwd=cwd, stdout=stream, stderr=subprocess.STDOUT,
                                           env=env, start_new_session=True)
                check["returncode"] = process.wait(timeout=timeout)
                check["status"] = "passed" if process.returncode == 0 else "failed"
            except subprocess.TimeoutExpired:
                check["status"] = "timeout"
                stream.write(f"\nDRIVER: exceeded {timeout}s; killing process group\n")
            except OSError as exc:
                check.update(status="error", error=str(exc))
                stream.write(f"\nDRIVER: {exc}\n")
            except BaseException:
                check["status"] = "interrupted"
                raise
            finally:
                # The official driver owns a separate database session. Give its
                # SIGTERM handler time to close that session before force-killing.
                if process is not None and (process.poll() is None or check["status"] != "passed"):
                    if process.poll() is None:
                        try:
                            os.killpg(process.pid, signal.SIGTERM)
                            process.wait(timeout=10)
                        except (ProcessLookupError, subprocess.TimeoutExpired):
                            pass
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    process.wait()
                    check.setdefault("returncode", process.returncode)
                check["seconds"] = round(time.monotonic() - started, 3)
                self.save()
        print(f"[{variant}] {name}: {check['status']} ({check['seconds']:.1f}s)", flush=True)
        return check["status"] == "passed"


def compiler_from_cache(build):
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if line.startswith("CMAKE_CXX_COMPILER:"):
            return line.split("=", 1)[1]
    raise RuntimeError("CMAKE_CXX_COMPILER missing from CMakeCache.txt")


def validate_variant(run, name, patches, base, official):
    output = run.output
    source, build = output / "sources" / name, output / "build" / name
    row = run.summary["variants"][name]

    def ignore(directory, names):
        return [entry for entry in names if entry in (".git", "__pycache__")
                or (Path(directory) / entry).resolve() == output]

    shutil.copytree(base, source, ignore=ignore)
    (source / "tests").mkdir(exist_ok=True)
    for validator in VALIDATORS:
        shutil.copy2(ROOT / "validation" / validator, source / "tests" / validator)
    for patch in patches:
        if not run.run(name, "patch_" + patch,
                       ["patch", "--batch", "--forward", "--fuzz=0", "-p1", "-i",
                        ROOT / "patches" / (patch + ".patch")], source, 30):
            return
    row["source"] = source_hashes(source)
    row["source_dir"], row["build_dir"] = str(source), str(build)
    if not run.run(name, "configure", ["cmake", "-S", source, "-B", build,
                                       "-DCMAKE_BUILD_TYPE=Release"], source, 120):
        return
    if not run.run(name, "build", ["cmake", "--build", build, "--parallel", "2"], source, 600):
        return
    binary = (build / "minivectordb").resolve(strict=True)
    compiler = compiler_from_cache(build)
    row.update(binary=str(binary), binary_sha256=sha256(binary), compiler=compiler)
    tests = source / "tests"
    py = sys.executable
    checks = [
        ("official_protocol", [py, official / "protocol_check" / "run.py", "--command",
                               json.dumps([str(binary), "--stdio", "--data-dir={data_dir}"]),
                               "--output", output / "protocol" / name], 300),
        ("sql_regressions", [py, tests / "run_cases.py", tests / "cases", "--binary", binary], 180),
        ("jsonl", [py, tests / "stdio_protocol.py", "--binary", binary], 90),
        ("query_boundaries", [py, tests / "query_boundaries.py", "--binary", binary, "-v"], 180),
        ("topk_memory", [py, tests / "topk_memory.py", "--binary", binary], 600),
        ("fuzz60", [py, tests / "fuzz.py", "--binary", binary, "--seed", "1", "--rounds", "60"], 300),
        ("filtered_correctness", [py, tests / "filtered_topk.py", "--binary", binary], 180),
    ]
    if "filtered" in patches:
        checks.append(("filtered_memory", [py, tests / "filtered_topk.py", "--binary", binary,
                                           "--memory"], 600))
    for check, command, timeout in checks:
        run.run(name, check, command, source, timeout)

    if name in ("wal", "all"):
        environment = dict(os.environ, CXX=compiler)
        run.run(name, "wal_serialization", [py, tests / "wal_serialization.py", "--baseline",
                                            output / "sources" / "base" / "src" / "storage.cpp",
                                            "--candidate", source / "src" / "storage.cpp"],
                source, 300, environment)
    if name in ("scan", "all"):
        flags = [compiler, "-std=c++17", "-O3", "-Wall", "-Wextra"]
        old_object, new_object = build / "scan_baseline.o", build / "scan_candidate.o"
        old_ok = run.run(name, "scan_compile_baseline",
                         flags + ["-Dmvd=scan_baseline", "-c", output / "sources" / "base" / "src" / "vecops.cpp",
                                  "-o", old_object], source, 120)
        new_ok = run.run(name, "scan_compile_candidate",
                         flags + ["-c", source / "src" / "vecops.cpp", "-o", new_object], source, 120)
        exact = build / "scan_exact_equivalence"
        if old_ok and new_ok and run.run(name, "scan_link",
                                        flags + [tests / "scan_exact_equivalence.cpp", old_object,
                                                 new_object, "-o", exact], source, 120):
            run.run(name, "scan_exact_equivalence", [exact], source, 180)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", type=Path, required=True)
    parser.add_argument("--official", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    base, official, output = args.base.resolve(), args.official.resolve(), args.output.resolve()
    required = [base / "CMakeLists.txt", official / "protocol_check" / "run.py",
                official / "mysqltest" / "manifest.json"]
    required += [base / "tests" / filename for filename in
                 ("run_cases.py", "stdio_protocol.py", "query_boundaries.py", "topk_memory.py", "fuzz.py")]
    required += [ROOT / "patches" / (name + ".patch") for name in ("wal", "filtered", "scan")]
    required += [ROOT / "validation" / name for name in VALIDATORS]
    for path in required:
        if not path.is_file():
            parser.error(f"missing required input: {path}")
    regressions = sorted((base / "tests" / "cases").glob("*.test"))
    if len(regressions) != 13 or any(not path.with_suffix(".result").is_file() for path in regressions):
        parser.error("expected the 13 SQL regressions with matching .result files")
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        parser.error("--output must be a new or empty directory")
    output.mkdir(parents=True, exist_ok=True)
    summary = {"status": "running", "base": str(base), "official": str(official),
               "platform": platform.platform(), "python": sys.version, "started_unix": time.time(),
               "base_source": source_hashes(base), "sql_regression_count": len(regressions),
               "input_sha256": {str(path): sha256(path) for path in required}, "variants": {}}
    run = Validation(output, summary)
    run.save()
    try:
        for name, patches in VARIANTS.items():
            row = {"patches": list(patches), "status": "running", "eligible_for_benchmark": False, "checks": []}
            summary["variants"][name] = row
            try:
                validate_variant(run, name, patches, base, official)
            except Exception as exc:
                row["error"] = f"{type(exc).__name__}: {exc}"
                print(f"[{name}] ERROR: {row['error']}", flush=True)
            finally:
                passed = ("binary_sha256" in row and "error" not in row and row["checks"]
                          and all(check["status"] == "passed" for check in row["checks"]))
                row.update(status="passed" if passed else "failed", eligible_for_benchmark=bool(passed))
                row["failed_checks"] = [check["name"] for check in row["checks"] if check["status"] != "passed"]
                if passed:
                    run.binaries[name] = row["binary"]
                run.save()
    except KeyboardInterrupt:
        summary["interrupted"] = True
        print("Validation interrupted; partial results saved", file=sys.stderr)
    finally:
        summary["finished_unix"] = time.time()
        summary["status"] = "passed" if len(run.binaries) == len(VARIANTS) else "failed"
        run.save()
    print(f"{len(run.binaries)}/{len(VARIANTS)} variants passed; reports: {output}", flush=True)
    return 0 if summary["status"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
