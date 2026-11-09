#!/usr/bin/env python3
"""MiniVectorDB 小规模性能基准（对齐题面 3.3：10k 行、128 维、1000 次 TopK、K=10、半过滤）。"""
import argparse
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import time


def gen_rows(n, dim, seed=42, ncat=100):
    rng = random.Random(seed)
    rows = []
    for i in range(1, n + 1):
        vec = [round(rng.uniform(-1.0, 1.0), 6) for _ in range(dim)]
        rows.append((i, "c%d" % (i % ncat), vec))
    return rows


def vec_text(v):
    return "[" + ",".join(repr(x) for x in v) + "]"


def start(binary, data_dir):
    proc = subprocess.Popen([binary, "--data-dir", data_dir],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, encoding="utf-8", bufsize=1)
    return proc


def send(proc, sql):
    proc.stdin.write(sql + ";\n")
    proc.stdin.flush()
    line = proc.stdout.readline()
    if not line:
        raise RuntimeError("server closed stdout: " + (proc.stderr.read() if proc.stderr else ""))
    return line.strip()


def mem_kb(pid):
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmHWM:"):
                    return int(line.split()[1])
    except FileNotFoundError:
        pass
    return 0


def io_bytes(pid):
    try:
        with open(f"/proc/{pid}/io") as f:
            d = {}
            for line in f:
                k, v = line.split(":")
                d[k.strip()] = int(v)
            return d.get("read_bytes", 0), d.get("write_bytes", 0)
    except FileNotFoundError:
        return 0, 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="./build/minivectordb")
    ap.add_argument("--n", type=int, default=10000)
    ap.add_argument("--dim", type=int, default=128)
    ap.add_argument("--queries", type=int, default=1000)
    ap.add_argument("--k", type=int, default=10)
    ap.add_argument("--batch", type=int, default=100)
    ap.add_argument("--filter-half", action="store_true", default=True)
    ap.add_argument("--trivial", action="store_true", help="用 SELECT id FROM bench LIMIT 1 测纯 IPC 开销")
    args = ap.parse_args()

    work = tempfile.mkdtemp(prefix="mvd_bench_")
    data_dir = os.path.join(work, "data")
    os.makedirs(data_dir)
    rows = gen_rows(args.n, args.dim)
    rng = random.Random(7)

    try:
        proc = start(args.binary, data_dir)
        send(proc, f"CREATE TABLE bench (id BIGINT PRIMARY KEY, cat VARCHAR(16), v VECTOR({args.dim}))")
        inserts = []
        for b in range(0, len(rows), args.batch):
            chunk = rows[b:b + args.batch]
            vals = ",".join("(%d,'%s','%s')" % (i, c, vec_text(v)) for i, c, v in chunk)
            inserts.append("INSERT INTO bench VALUES " + vals)
        t0 = time.perf_counter()
        for sql in inserts:
            r = send(proc, sql)
            assert "affected_rows" in r, r
        t_write = time.perf_counter() - t0
        peak_write = mem_kb(proc.pid)
        rb_w, wb_w = io_bytes(proc.pid)

        # 查询阶段
        qs = []
        for q in range(args.queries):
            if args.trivial:
                qs.append(("SELECT id FROM bench LIMIT 1", None, None))
                continue
            qv = [round(rng.uniform(-1.0, 1.0), 6) for _ in range(args.dim)]
            if q % 2 == 0:
                cat = "c%d" % rng.randrange(100)
                sql = "SELECT id, L2_DISTANCE(v,'%s') AS d FROM bench WHERE cat = '%s' ORDER BY d LIMIT %d" % (
                    vec_text(qv), cat, args.k)
                qs.append((sql, qv, cat))
            else:
                sql = "SELECT id, L2_DISTANCE(v,'%s') AS d FROM bench ORDER BY d LIMIT %d" % (
                    vec_text(qv), args.k)
                qs.append((sql, qv, None))
        t0 = time.perf_counter()
        for sql, _, _ in qs:
            r = send(proc, sql)
            assert "columns" in r, r[:200]
        t_query = time.perf_counter() - t0
        peak = mem_kb(proc.pid)
        rb, wb = io_bytes(proc.pid)

        # 正确性：抽 5 条与暴力法比对
        by_id = {i: (c, v) for i, c, v in rows}
        bad = 0
        for sql, qv, cat in ([] if args.trivial else qs[:5]):
            resp = json.loads(send(proc, sql))
            got = [row[0] for row in resp["rows"]]
            cand = [(i, sum((a - b) ** 2 for a, b in zip(v, qv))) for i, c, v in rows
                    if cat is None or c == cat]
            cand.sort(key=lambda x: (x[1], x[0]))
            want = [i for i, _ in cand[:args.k]]
            if got != want:
                bad += 1
                print("MISMATCH", sql[:60], got[:5], want[:5])
        proc.stdin.close()
        proc.wait(timeout=10)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    print(f"rows={args.n} dim={args.dim} queries={args.queries} k={args.k}")
    print(f"T_write={t_write*1000:.1f} ms")
    print(f"T_query={t_query*1000:.1f} ms  ({t_query/args.queries*1e6:.1f} us/query)")
    print(f"peak_rss={peak/1024:.1f} MiB (write phase {peak_write/1024:.1f} MiB)")
    print(f"process io read={rb/1048576:.2f} MiB write={wb/1048576:.2f} MiB")
    print(f"topk mismatches={bad}")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
