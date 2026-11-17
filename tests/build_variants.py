#!/usr/bin/env python3
"""Build the historical IEEE CRC32/response-buffer four-way campaign.

This intentionally requires changes only in common.h/main.cpp, as in the frozen
2026-11-15 a285f2e/9db50ce archives. It is not a constructor for the later
CRC-32C integration (whose IEEE optimization was withdrawn).
"""

import argparse
import hashlib
import json
import os
import platform
import shlex
import shutil
import subprocess
import sys
from pathlib import Path


CRC_FILE = "src/common.h"
BUFFER_FILE = "src/main.cpp"
VARIANTS = {
    "baseline": (),
    "crc": (CRC_FILE,),
    "buffer": (BUFFER_FILE,),
    "combined": (CRC_FILE, BUFFER_FILE),
}
COPY_IGNORE = (".git", "build", "__pycache__")


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_hashes(root):
    src = root / "src"
    cmake = root / "CMakeLists.txt"
    if not src.is_dir() or not cmake.is_file():
        raise ValueError(f"{root} must contain src/ and CMakeLists.txt")

    hashes = {"CMakeLists.txt": sha256(cmake)}
    for folder, directories, files in os.walk(src):
        directories[:] = [name for name in directories if name not in COPY_IGNORE]
        for name in sorted(files):
            if name not in COPY_IGNORE:
                path = Path(folder) / name
                hashes[path.relative_to(root).as_posix()] = sha256(path)
    return hashes


def verify_hashes(actual, expected, label):
    differences = sorted(
        name for name in actual.keys() | expected.keys()
        if actual.get(name) != expected.get(name)
    )
    if differences:
        raise ValueError(f"{label} differs at {', '.join(differences)}")


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def run_logged(args, log, *, capture=False):
    print(f"$ {shlex.join(args)}", file=log, flush=True)
    if capture:
        result = subprocess.run(
            args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
        )
        print(result.stdout, file=log, end="", flush=True)
        result.check_returncode()
        return result.stdout.strip()
    subprocess.run(args, stdout=log, stderr=subprocess.STDOUT, check=True)
    return None


def build(source, binary, log, mac_helper):
    if mac_helper is not None:
        compiler = run_logged(
            ["xcrun", "--sdk", "macosx", "clang++", "--version"], log, capture=True
        )
        run_logged(["bash", "--", str(mac_helper), str(source), "worktree", str(binary)], log)
        return {"compiler_version": compiler}

    build_dir = binary.parent / "build"
    cmake_version = run_logged(["cmake", "--version"], log, capture=True)
    run_logged(
        [
            "cmake", "-S", str(source), "-B", str(build_dir),
            "-G", "Unix Makefiles", "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        ],
        log,
    )
    cache = (build_dir / "CMakeCache.txt").read_text(encoding="utf-8")
    compiler_path = next(
        (line.split("=", 1)[1] for line in cache.splitlines()
         if line.startswith("CMAKE_CXX_COMPILER:")),
        None,
    )
    if not compiler_path:
        raise ValueError(f"No C++ compiler recorded in {build_dir / 'CMakeCache.txt'}")
    compiler = run_logged([compiler_path, "--version"], log, capture=True)
    run_logged(["cmake", "--build", str(build_dir), "-j2", "--verbose"], log)
    shutil.copy2(build_dir / "minivectordb", binary)
    return {
        "compiler_path": compiler_path,
        "compiler_version": compiler,
        "cmake_version": cmake_version,
        "compile_commands": str(build_dir / "compile_commands.json"),
    }


def build_variants(baseline, candidate, output, mac_helper):
    for source in (baseline, candidate):
        if output == source or source in output.parents:
            raise ValueError(f"Output directory must be outside the input source tree {source}")

    base_hashes = source_hashes(baseline)
    candidate_hashes = source_hashes(candidate)
    combined_hashes = base_hashes.copy()
    for name in VARIANTS["combined"]:
        if name not in candidate_hashes or name not in base_hashes:
            raise ValueError(f"Both source trees must contain {name}")
        if base_hashes[name] == candidate_hashes[name]:
            raise ValueError(f"Expected different baseline and candidate hashes for {name}")
        combined_hashes[name] = candidate_hashes[name]
    verify_hashes(candidate_hashes, combined_hashes, "Candidate source")

    output.mkdir(parents=True)
    info = {
        "platform": platform.platform(),
        "architecture": platform.machine(),
        "python_version": platform.python_version(),
        "baseline_source": str(baseline),
        "candidate_source": str(candidate),
        "baseline_sha256": base_hashes,
        "candidate_sha256": candidate_hashes,
        "variants": {},
    }
    if mac_helper is not None:
        info["mac_compat_build"] = str(mac_helper)
        info["mac_compat_build_sha256"] = sha256(mac_helper)
    info_path = output / "build-info.json"
    write_json(info_path, info)

    binaries = {}
    for name, replacements in VARIANTS.items():
        variant_dir = output / name
        source = variant_dir / "source"
        binary = variant_dir / "minivectordb"
        log_path = variant_dir / "build.log"
        print(f"Building {name} ({log_path})", flush=True)
        shutil.copytree(baseline, source, ignore=shutil.ignore_patterns(*COPY_IGNORE))
        expected = base_hashes.copy()
        for relative_path in replacements:
            shutil.copy2(candidate / relative_path, source / relative_path)
            expected[relative_path] = candidate_hashes[relative_path]

        verify_hashes(source_hashes(source), expected, f"{name} source")
        variant_info = {
            "source": str(source),
            "source_sha256": expected,
            "build_log": str(log_path),
        }
        info["variants"][name] = variant_info
        write_json(info_path, info)
        with log_path.open("w", encoding="utf-8") as log:
            print(f"Platform: {info['platform']} ({info['architecture']})", file=log)
            for path, digest in sorted(expected.items()):
                print(f"SHA-256 {digest}  {path}", file=log)
            variant_info.update(build(source, binary, log, mac_helper))
            verify_hashes(source_hashes(source), expected, f"{name} source after build")
            if not binary.is_file() or not os.access(binary, os.X_OK):
                raise ValueError(f"Build did not produce an executable at {binary}")
            binary_hash = sha256(binary)
            print(f"SHA-256 {binary_hash}  {binary}", file=log)

        variant_info.update(binary=str(binary), binary_sha256=binary_hash)
        write_json(info_path, info)
        binaries[name] = str(binary)

    binaries_path = output / "binaries.json"
    write_json(binaries_path, binaries)
    return binaries_path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-source", type=Path, required=True, help="Extracted baseline tree")
    parser.add_argument("--candidate-source", type=Path, required=True, help="Extracted candidate tree")
    parser.add_argument("--output", type=Path, required=True, help="New directory for sources and builds")
    parser.add_argument("--mac-compat-build", type=Path, help="macOS compatibility build shell script")
    args = parser.parse_args()

    mac_helper = args.mac_compat_build.resolve() if args.mac_compat_build else None
    if sys.platform == "darwin":
        if mac_helper is None or not mac_helper.is_file():
            parser.error("macOS requires --mac-compat-build pointing to a shell script")
    elif sys.platform.startswith("linux"):
        if mac_helper is not None:
            parser.error("--mac-compat-build is only used on macOS")
    else:
        parser.error("This script supports Linux and macOS")

    try:
        manifest = build_variants(
            args.baseline_source.resolve(),
            args.candidate_source.resolve(),
            args.output.resolve(),
            mac_helper,
        )
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Build variants failed: {error}", file=sys.stderr)
        return 1
    print(manifest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
