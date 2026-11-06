#!/usr/bin/env python3
"""Bounded bridge research benchmark; no SQL/contest performance claim.

The fixture follows run_gpu_smoke.sh's seed/normalization/probe definition.
Each paired group is at most five alternating pairs, in fresh child processes.
"""
import argparse
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import resource
import subprocess
import sys
import threading
import time

os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")
os.environ.setdefault("OMP_NUM_THREADS", "8")
import numpy as np


def sha(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def prepare(args):
    args.data.mkdir(parents=True, exist_ok=False)
    rng = np.random.default_rng(1314)
    base = rng.normal(size=(args.n, args.dim)).astype(np.float32)
    base /= np.linalg.norm(base, axis=1, keepdims=True)
    rows = np.linspace(0, args.n - 1, 100, dtype=np.int64)
    query = base[rows] + rng.normal(scale=.002, size=(100, args.dim)).astype(np.float32)
    query /= np.linalg.norm(query, axis=1, keepdims=True)
    # Independent FP64 truth avoids expanded-FP32 cancellation near the cutoff.
    truth = np.empty((100, 100), dtype=np.int32)
    bd = base.astype(np.float64)
    bn = np.sum(bd * bd, axis=1)
    for q, vector in enumerate(query.astype(np.float64)):
        dist = bn + np.sum(vector * vector) - 2 * (bd @ vector)
        chosen = np.argpartition(dist, 99)[:100]
        truth[q] = chosen[np.argsort(dist[chosen], kind="stable")]
    for name, array in [("base", base), ("query", query), ("truth", truth)]:
        np.save(args.data / (name + ".npy"), array)
    manifest = dict(seed=1314, n=args.n, dim=args.dim, nq=100, truth_k=100,
                    fixture="run_gpu_smoke generator; scaled n/dim; independent FP64 truth",
                    hashes={p.name: sha(p) for p in args.data.glob("*.npy")})
    (args.data / "manifest.json").write_text(json.dumps(manifest, indent=2))
    print(json.dumps(manifest), flush=True)


def worker(args):
    # This benchmark deliberately targets visible device0, including its sampler.
    os.environ.update(SEEKDB_CUVS_BF_GEMM=str(args.gemm), SEEKDB_CUVS_TIME="1", SEEKDB_CUVS_GPU="0")
    os.environ["SEEKDB_CUVS_GEMM_FUSED_NORM"] = str(args.fused)
    data = args.data
    manifest = json.loads((data / "manifest.json").read_text())
    for name, digest in manifest["hashes"].items():
        assert sha(data / name) == digest, name
    base = np.load(data / "base.npy")
    query = np.load(data / "query.npy")
    truth = np.load(data / "truth.npy")
    n, dim = base.shape
    lib = C.CDLL(str(args.bridge.resolve()))
    fp, up = C.POINTER(C.c_float), C.POINTER(C.c_uint32)
    lib.seekdb_cuvs_build.argtypes = [fp, C.c_long, C.c_long]
    lib.seekdb_cuvs_build.restype = C.c_void_p
    lib.seekdb_cuvs_free.argtypes = [C.c_void_p]
    lib.seekdb_cuvs_search.argtypes = [C.c_void_p, fp, C.c_long, C.c_long, up, fp]
    lib.seekdb_cuvs_search.restype = C.c_int
    hip = C.CDLL("/opt/rocm/lib/libamdhip64.so")
    hip.hipMemGetInfo.argtypes = [C.POINTER(C.c_size_t), C.POINTER(C.c_size_t)]
    assert hip.hipSetDevice(0) == 0
    bdf = C.create_string_buffer(32)
    hip.hipDeviceGetPCIBusId.argtypes = [C.POINTER(C.c_char), C.c_int, C.c_int]
    assert hip.hipDeviceGetPCIBusId(bdf, len(bdf), 0) == 0

    def memory():
        free, total = C.c_size_t(), C.c_size_t()
        assert hip.hipMemGetInfo(C.byref(free), C.byref(total)) == 0
        return total.value - free.value

    def sync():
        assert hip.hipDeviceSynchronize() == 0, "HIP synchronization failed"

    def build():
        sync()
        started = time.perf_counter()
        handle = lib.seekdb_cuvs_build(base.ctypes.data_as(fp), n, dim)
        sync()
        assert handle, "bridge build failed"
        return handle, (time.perf_counter() - started) * 1000

    def search(handle, vectors, k):
        vectors = np.ascontiguousarray(vectors)
        ids = np.empty((len(vectors), k), dtype=np.uint32)
        distances = np.empty(ids.shape, dtype=np.float32)
        rc = lib.seekdb_cuvs_search(handle, vectors.ctypes.data_as(fp), len(vectors), k,
                                   ids.ctypes.data_as(up), distances.ctypes.data_as(fp))
        assert rc == 0, f"search rc={rc}"
        sync()
        return ids, distances

    initial = memory()
    samples, stop, monitor_errors = [], threading.Event(), []

    def monitor():
        try:
            assert hip.hipSetDevice(0) == 0
            while not stop.is_set():
                samples.append((time.monotonic_ns(), memory()))
                stop.wait(.002)
        except BaseException as error:
            monitor_errors.append(repr(error))

    monitor_thread = threading.Thread(target=monitor, daemon=True)
    monitor_thread.start()
    handle = None
    try:
        handle, cold_ms = build()
        started = time.perf_counter()
        search(handle, query, 100)
        cold_search_ms = (time.perf_counter() - started)*1000
        lib.seekdb_cuvs_free(handle)
        handle = None
        sync()
        after_warmup_free = memory()
        handle, build_ms = build()
        persistent = memory()
        ids, distances = search(handle, query, 100)
        assert np.all(ids < n) and np.isfinite(distances).all()
        assert all(len(set(row.tolist())) == 100 for row in ids), "duplicate neighbor IDs"
        recalls = {str(k): sum(len(set(row[:k]) & set(gt[:k])) for row, gt in zip(ids, truth)) / (100*k)
                   for k in (10, 100)}
        # Check distances against the precision the bridge actually stores.
        half = dim <= 128
        stored_base = base.astype(np.float16).astype(np.float64) if half else base.astype(np.float64)
        stored_query = query.astype(np.float16).astype(np.float64) if half else query.astype(np.float64)
        exact = np.sum((stored_base[ids] - stored_query[:, None, :])**2, axis=2)
        max_error = float(np.max(np.abs(exact - distances)))
        assert max_error < .001, ("distance error", max_error)
        assert min(recalls.values()) >= .75, ("existing smoke recall gate", recalls)
        latency = []
        search(handle, query[:1], 10)
        for vector in query[:32]:
            started = time.perf_counter()
            search(handle, vector[None, :], 10)
            latency.append((time.perf_counter() - started)*1000)
        batch = {}
        for batch_size in (16, 100):
            search(handle, query[:batch_size], 10)
            started = time.perf_counter()
            for _ in range(4):
                search(handle, query[:batch_size], 10)
            seconds = time.perf_counter() - started
            batch[str(batch_size)] = dict(qps=batch_size*4/seconds, total_ms=seconds*1000)
        lib.seekdb_cuvs_free(handle)
        handle = None
        sync()
        after_free = memory()
        assert not monitor_errors, monitor_errors
        result = dict(implementation=args.implementation, bridge_sha256=sha(args.bridge),
                      hip_device=0, pci_bdf=bdf.value.decode(),
                      data_manifest_sha256=sha(data / "manifest.json"), gemm=args.gemm, fused=args.fused,
                      cold_build_ms=cold_ms, warm_build_ms=build_ms, recall=recalls,
                      cold_batch100_search_ms=cold_search_ms,
                      max_stored_distance_error=max_error, single_latency_ms=latency,
                      single_qps=1000/float(np.mean(latency)),
                      latency_percentiles_ms={str(q): float(np.percentile(latency, q)) for q in (50, 95, 99)},
                      batch=batch, vram_initial=initial, vram_after_warmup_free=after_warmup_free,
                      vram_index_persistent=persistent, vram_after_free=after_free,
                      vram_free_delta=after_free-after_warmup_free,
                      sampled_peak_vram=max([initial, persistent, after_free] + [v for _, v in samples]),
                      vram_sample_interval_ms=2, max_rss_kib=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss)
        assert after_free-after_warmup_free < 16*1024**2, ("unexplained device growth", result)
        print(json.dumps(result), flush=True)
    finally:
        if handle:
            lib.seekdb_cuvs_free(handle)
        stop.set()
        monitor_thread.join(timeout=2)


def paired(args):
    args.output.mkdir(parents=True, exist_ok=False)
    results = []
    for pair in range(args.pairs):
        if args.deadline_epoch and time.time() + 120 > args.deadline_epoch:
            raise RuntimeError("Acceptance reserve reached; no new paired sample")
        for variant in ((0, 1) if pair % 2 == 0 else (1, 0)):
            gemm = 1 if args.compare_fusion else variant
            fused = variant if args.compare_fusion else 0
            bridge = args.reference_bridge if variant == 0 and args.reference_bridge else args.bridge
            name = f"pair-{pair}-variant-{variant}"
            telemetry = subprocess.run(
                ["/opt/rocm/bin/rocm-smi", "--showtemp", "--showuse", "--showpower", "--json"],
                capture_output=True, text=True, timeout=15)
            (args.output / (name+".telemetry.json")).write_text(json.dumps(
                dict(command="rocm-smi --showtemp --showuse --showpower --json",
                     rc=telemetry.returncode, stdout=telemetry.stdout, stderr=telemetry.stderr)))
            command = [sys.executable, __file__, "worker", "--data", str(args.data),
                       "--bridge", str(bridge), "--gemm", str(gemm), "--fused", str(fused),
                       "--implementation", args.implementation]
            with (args.output / (name+".log")).open("w") as log:
                run = subprocess.run(command, stdout=subprocess.PIPE, stderr=log, text=True, timeout=240)
            (args.output / (name+".stdout")).write_text(run.stdout)
            assert run.returncode == 0, (name, run.returncode)
            trace = (args.output / (name+".log")).read_text()
            expected_path = "bf-gemm:" if gemm else "after_bf_shard"
            assert expected_path in trace and "falling back" not in trace, (name, "wrong execution path")
            if args.compare_fusion and variant:
                import re
                assert any(int(v) > 0 for v in re.findall(r"fused_tiles=(\d+)", trace)), "fused path did not execute"
            row = json.loads(run.stdout.strip().splitlines()[-1])
            row.update(pair=pair, group=args.group, label=name, variant=variant)
            with (args.output / "raw.jsonl").open("a") as stream:
                stream.write(json.dumps(row)+"\n")
            results.append(row)
            print(json.dumps({k: row[k] for k in ("label", "warm_build_ms", "recall", "sampled_peak_vram")}), flush=True)
    ratios = [next(r["warm_build_ms"] for r in results if r["pair"]==p and not r["variant"])/
              next(r["warm_build_ms"] for r in results if r["pair"]==p and r["variant"]) for p in range(args.pairs)]
    summary = dict(group=args.group, paired_speedups=ratios, median_speedup=float(np.median(ratios)),
                   improvement_percent=float((1-1/np.median(ratios))*100),
                   note="Bridge-only; no SQL or contest speedup established")
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary), flush=True)


def main():
    parser = argparse.ArgumentParser()
    subs = parser.add_subparsers(dest="command", required=True)
    p = subs.add_parser("prepare")
    p.add_argument("--data", type=Path, required=True)
    p.add_argument("--n", type=int, default=250000)
    p.add_argument("--dim", type=int, choices=(128, 256), default=128)
    for name in ("worker", "paired"):
        p = subs.add_parser(name)
        p.add_argument("--data", type=Path, required=True)
        p.add_argument("--bridge", type=Path, required=True)
        p.add_argument("--implementation", default="f97bc2823+hipVS-fp16-compat")
        if name == "worker":
            p.add_argument("--gemm", type=int, choices=(0, 1), required=True)
            p.add_argument("--fused", type=int, choices=(0, 1), default=0)
        else:
            p.add_argument("--output", type=Path, required=True)
            p.add_argument("--pairs", type=int, choices=range(1, 6), default=5)
            p.add_argument("--group", type=int, choices=(1, 2, 3), required=True)
            p.add_argument("--deadline-epoch", type=float)
            p.add_argument("--compare-fusion", action="store_true")
            p.add_argument("--reference-bridge", type=Path)
    args = parser.parse_args()
    if args.command == "prepare":
        assert args.n >= 100
    globals()[args.command](args)


if __name__ == "__main__":
    main()
