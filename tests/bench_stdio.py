#!/usr/bin/env python3
"""JSON Lines 协议版性能基准（对齐平台评测驱动：--stdio、ready 握手、id 回传）。

负载与题面 3.3 一致：10k 行、128 维、1000 次 TopK、K=10、半数带等值过滤。
多轮运行输出每轮 T_write / T_query，便于在 CI 上对比优化前后。
"""
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


class StdioClient:
    def __init__(self, binary, data_dir):
        self.proc = subprocess.Popen(
            [binary, "--stdio", "--data-dir", data_dir],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, encoding="utf-8", bufsize=1)
        self.next_id = 1
        line = self.proc.stdout.readline()
        if not line or json.loads(line).get("type") != "ready":
            raise RuntimeError("no ready handshake: " + (self.proc.stderr.read() or ""))

    def send(self, sql):
        rid = self.next_id
        self.next_id += 1
        req = {"id": rid, "op": "execute", "sql": sql, "search_mode": "exact"}
        self.proc.stdin.write(json.dumps(req, separators=(",", ":")) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("server closed stdout: " + (self.proc.stderr.read() or ""))
        resp = json.loads(line)
        if resp.get("id") != rid:
            raise RuntimeError("id mismatch: %r" % resp)
        if not resp.get("ok"):
            raise RuntimeError("execute failed: %r" % resp)
        return resp

    def close(self):
        try:
            self.proc.stdin.write(json.dumps({"id": 0, "op": "shutdown"}) + "\n")
            self.proc.stdin.flush()
            self.proc.stdout.readline()
        except Exception:
            pass
        self.proc.stdin.close()
        self.proc.wait(timeout=10)


def mem_kb(pid):
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmHWM:"):
                    return int(line.split()[1])
    except FileNotFoundError:
        pass
    return 0


def run_once(binary, n, dim, queries, k, batch, work):
    data_dir = os.path.join(work, "data")
    os.makedirs(data_dir, exist_ok=True)
    rows = gen_rows(n, dim)
    rng = random.Random(7)
    client = StdioClient(binary, data_dir)
    try:
        client.send("CREATE TABLE bench (id BIGINT PRIMARY KEY, cat VARCHAR(16), v VECTOR(%d))" % dim)
        inserts = []
        for b in range(0, len(rows), batch):
            chunk = rows[b:b + batch]
            vals = ",".join("(%d,'%s','%s')" % (i, c, vec_text(v)) for i, c, v in chunk)
            inserts.append("INSERT INTO bench VALUES " + vals)
        t0 = time.perf_counter()
        for sql in inserts:
            r = client.send(sql)
            assert r.get("affected_rows") == batch, r
        t_write = time.perf_counter() - t0
        peak_write = mem_kb(client.proc.pid)

        qs = []
        for q in range(queries):
            qv = [round(rng.uniform(-1.0, 1.0), 6) for _ in range(dim)]
            if q % 2 == 0:
                cat = "c%d" % rng.randrange(100)
                qs.append(("SELECT id, L2_DISTANCE(v,'%s') AS d FROM bench WHERE cat = '%s' "
                           "ORDER BY d LIMIT %d" % (vec_text(qv), cat, k), qv, cat))
            else:
                qs.append(("SELECT id, L2_DISTANCE(v,'%s') AS d FROM bench ORDER BY d LIMIT %d"
                           % (vec_text(qv), k), qv, None))
        t0 = time.perf_counter()
        for sql, _, _ in qs:
            r = client.send(sql)
            assert "rows" in r and len(r["rows"]) > 0, r
        t_query = time.perf_counter() - t0
        peak = mem_kb(client.proc.pid)

        # 正确性抽查：前 5 条与暴力法比对（含 float32 距离复核）
        import math
        import struct

        def f32(x):
            return struct.unpack("f", struct.pack("f", x))[0]

        by_id = {i: (c, v) for i, c, v in rows}
        bad = 0
        for sql, qv, cat in qs[:5]:
            resp = client.send(sql)
            got = [row[0] for row in resp["rows"]]
            got_dist = {row[0]: row[1] for row in resp["rows"]}
            cand = [(i, sum((a - b) ** 2 for a, b in zip(v, qv))) for i, c, v in rows
                    if cat is None or c == cat]
            cand.sort(key=lambda x: (x[1], x[0]))
            want = [i for i, _ in cand[:k]]
            if got != want:
                bad += 1
                print("MISMATCH", sql[:60], got[:5], want[:5])
                print("  got detail:", [(i, got_dist.get(i)) for i in got])
                print("  want detail:", [(i, math.sqrt(d)) for i, d in cand[:k]])
                for i in got:
                    c, v = by_id.get(i, (None, None))
                    f32_dist = math.sqrt(sum((f32(a) - f32(b)) ** 2
                                             for a, b in zip(v, qv)))
                    print(f"  got row id={i} cat={c} engine_dist={got_dist.get(i)} "
                          f"f32_dist={f32_dist:.9g} matches_filter={cat is None or c == cat}")
                for i in want[:3]:
                    c, v = by_id[i]
                    print(f"  want row id={i} cat={c} "
                          f"matches_filter={cat is None or c == cat}")
    finally:
        client.close()
        shutil.rmtree(os.path.dirname(data_dir), ignore_errors=True)
    return t_write, t_query, peak_write, peak, bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="./build/minivectordb")
    ap.add_argument("--n", type=int, default=10000)
    ap.add_argument("--dim", type=int, default=128)
    ap.add_argument("--queries", type=int, default=1000)
    ap.add_argument("--k", type=int, default=10)
    ap.add_argument("--batch", type=int, default=100)
    ap.add_argument("--runs", type=int, default=3)
    args = ap.parse_args()

    results = []
    for r in range(args.runs):
        work = tempfile.mkdtemp(prefix="mvd_bench_stdio_")
        try:
            tw, tq, pw, pk, bad = run_once(args.binary, args.n, args.dim, args.queries,
                                           args.k, args.batch, work)
        finally:
            shutil.rmtree(work, ignore_errors=True)
        results.append((tw, tq))
        print("run %d: T_write=%.1f ms  T_query=%.1f ms (%.1f us/query)  "
              "peak_rss=%.1f MiB (write %.1f MiB)  mismatches=%d"
              % (r + 1, tw * 1e3, tq * 1e3, tq / args.queries * 1e6, pk / 1024, pw / 1024, bad))
        if bad:
            print("CORRECTNESS FAILURE")
            return 1

    tws = sorted(tw for tw, _ in results)
    tqs = sorted(tq for _, tq in results)
    med_w = tws[len(tws) // 2]
    med_q = tqs[len(tqs) // 2]
    print("median: T_write=%.1f ms  T_query=%.1f ms (%.1f us/query)"
          % (med_w * 1e3, med_q * 1e3, med_q / args.queries * 1e6))
    return 0


if __name__ == "__main__":
    sys.exit(main())
