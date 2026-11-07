"""Build one diagnostic binary and restore the exact source preimages."""
import fcntl
import gzip
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

from reference_probe import Session, checked, command, sha, stop_group, terminate, write_json


def main():
    signal.signal(signal.SIGTERM, terminate)
    work = Path(__file__).resolve().parent
    ref = Session(work / "reference-manifest.json")
    ref.preflight()
    request = json.loads((work / "cold-diagnostic-source.json").read_text())
    stage = work / "cold-diag-stage"
    stage.mkdir()
    preimages = work / "cold-diag-preimages"
    preimages.mkdir()
    src, run = ref.src, ref.run
    patch = gzip.decompress((work / "cold-diagnostic.patch.gz").read_bytes())
    import hashlib
    if hashlib.sha256(patch).hexdigest() != request["patch_sha256"]:
        raise RuntimeError("diagnostic patch identity mismatch")
    names = ["src/storage/fts/dict/ob_ft_range_dict.cpp", "src/storage/fts/dict/ob_ft_dat_dict.cpp"]
    if set(names) != set(request["source_before"]) or set(names) != set(request["source_after"]):
        raise RuntimeError("unexpected diagnostic file list")
    for name in names:
        target = src / name
        if target.is_symlink() or sha(target) != request["source_before"][name]:
            raise RuntimeError("preimage mismatch " + name)
        for out in (stage, preimages):
            f = out / name
            f.parent.mkdir(parents=True, exist_ok=True)
            with f.open("xb") as stream:
                stream.write(target.read_bytes())
    log = subprocess.run(["patch", "-p1", "--batch", "--fuzz=0"], cwd=stage, input=patch,
                         capture_output=True, timeout=10)
    with (work / "cold-diag-patch-apply.log").open("xb") as f:
        f.write(log.stdout + log.stderr)
    checked(log)
    for name in names:
        if sha(stage / name) != request["source_after"][name]:
            raise RuntimeError("patched source identity mismatch " + name)

    compiler = Path("/home/xiazihan/benchmark/seekdb-verify-20261104/toolchain")
    env = dict(os.environ, RUSTUP_HOME=str(compiler / "rustup"),
               CARGO_HOME=str(compiler / "cargo"), CARGO_NET_OFFLINE="true",
               CARGO_BUILD_JOBS="2", CMAKE_BUILD_PARALLEL_LEVEL="2")
    env["PATH"] = str(compiler / "cargo/bin") + ":" + env["PATH"]
    argv = ["bash", "build.sh", "release", "-DCMAKE_PROJECT_INCLUDE=" + str(run / "conservative_link.cmake"),
            "--make", "-j2"]
    record = dict(argv=argv, patch_sha256=request["patch_sha256"], instrumented_source_sha256=request["source_after"],
                  began_ns=time.time_ns(), sources_restored={}, guard_checks=[], rc=None)
    p = None
    edited = []
    try:
        with (work / "cold-diag-build.lock").open("x") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            if checked(command(["ss", "-ltnH", "sport = :2920"])).stdout:
                raise RuntimeError("test listener present before build")
            for name in names:
                target = src / name
                if sha(target) != request["source_before"][name]:
                    raise RuntimeError("source changed since preflight " + name)
                target.write_bytes((stage / name).read_bytes())
                edited.append(name)
            start = time.monotonic()
            with (work / "cold-diag-build.log").open("xb") as logf:
                p = subprocess.Popen(argv, cwd=src, env=env, stdin=subprocess.DEVNULL,
                                     stdout=logf, stderr=subprocess.STDOUT, start_new_session=True)
                while p.poll() is None:
                    try:
                        p.wait(timeout=30)
                    except subprocess.TimeoutExpired:
                        record["guard_checks"].append(ref.guard())
                        if time.monotonic() - start > 720:
                            raise TimeoutError("diagnostic incremental build exceeded 12 minutes")
            record["rc"] = p.returncode
            if p.returncode:
                raise RuntimeError("diagnostic build exit " + str(p.returncode))
            binary = work / "seekdb-cold-diag"
            with (src / "build_release/src/observer/seekdb").open("rb") as fr, binary.open("xb") as fw:
                shutil.copyfileobj(fr, fw, length=8 << 20)
            binary.chmod(0o700)
            record["diagnostic_binary"] = str(binary)
            record["diagnostic_sha256"] = sha(binary)
            record["build_cache_path"] = str(src / "build_release/src/observer/seekdb")
            record["bytes"] = binary.stat().st_size
    finally:
        if p is not None:
            stop_group(p)
        for name in reversed(edited):
            target = src / name
            if sha(target) != request["source_after"][name]:
                record["sources_restored"][name] = "not restored because current bytes differ from this patch"
                continue
            target.write_bytes((preimages / name).read_bytes())
            record["sources_restored"][name] = sha(target)
        record["ended_ns"] = time.time_ns()
        write_json(work / "cold-diag-build.json", record)
    if record["sources_restored"] != request["source_before"]:
        raise RuntimeError("verify protected source restoration")
    cfg = dict(ref.cfg, role="diagnostic", diagnostic_source=request["source_after"],
               reference=dict(path=record["diagnostic_binary"], sha256=record["diagnostic_sha256"]))
    write_json(work / "cold-diag-manifest.json", cfg)
    print("DIAGNOSTIC_BUILD_COMPLETE", record["diagnostic_sha256"], flush=True)


if __name__ == "__main__":
    main()
