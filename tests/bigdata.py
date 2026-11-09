#!/usr/bin/env python3
"""大数据量冒烟：验证流式存储、文件增长、重启恢复与内存占用（默认 20 万行 × 32 维）。"""
import argparse
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import time


def mem_kb(pid):
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmHWM:"):
                    return int(line.split()[1])
    except FileNotFoundError:
        pass
    return 0


def start(binary, data_dir):
    return subprocess.Popen([binary, "--data-dir", data_dir],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, encoding="utf-8", bufsize=1)


def send(p, sql):
    p.stdin.write(sql + ";\n")
    p.stdin.flush()
    line = p.stdout.readline()
    if line == "":
        raise RuntimeError("closed: " + (p.stderr.read() if p.stderr else ""))
    return json.loads(line)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="./build/minivectordb")
    ap.add_argument("--n", type=int, default=200000)
    ap.add_argument("--dim", type=int, default=32)
    ap.add_argument("--batch", type=int, default=1000)
    args = ap.parse_args()

    rng = random.Random(11)
    work = tempfile.mkdtemp(prefix="mvd_big_")
    data_dir = os.path.join(work, "data")
    os.makedirs(data_dir)
    p = start(args.binary, data_dir)
    send(p, f"CREATE TABLE big (id BIGINT PRIMARY KEY, v VECTOR({args.dim}))")

    t0 = time.perf_counter()
    for b in range(0, args.n, args.batch):
        rows = []
        for i in range(b + 1, min(b + args.batch, args.n) + 1):
            vec = ",".join("%.5f" % rng.uniform(-1, 1) for _ in range(args.dim))
            rows.append("(%d,'[%s]')" % (i, vec))
        r = send(p, "INSERT INTO big VALUES " + ",".join(rows))
        assert "affected_rows" in r, r
    t_write = time.perf_counter() - t0
    peak_write = mem_kb(p.pid)
    size = os.path.getsize(os.path.join(data_dir, "t_1.rows"))

    # 重启：测量恢复时间
    p.stdin.close()
    p.wait(timeout=60)
    t0 = time.perf_counter()
    p = start(args.binary, data_dir)
    r = send(p, "SELECT id FROM big ORDER BY id LIMIT 1")
    t_recover = time.perf_counter() - t0
    peak = mem_kb(p.pid)

    # 全表计数（扫描全部槽位）与随机点查
    t0 = time.perf_counter()
    r = send(p, "SELECT id, v FROM big ORDER BY id LIMIT %d" % args.n)
    t_scan = time.perf_counter() - t0
    assert len(r["rows"]) == args.n, len(r["rows"])
    last = r["rows"][-1][0]
    assert last == args.n, last

    p.stdin.close()
    p.wait(timeout=60)
    shutil.rmtree(work, ignore_errors=True)
    print(f"rows={args.n} dim={args.dim} data={size/1048576:.1f} MiB")
    print(f"T_write={t_write:.1f}s write_peak={peak_write/1024:.1f} MiB")
    print(f"T_recover_to_first_row={t_recover*1000:.1f}ms recover_peak={peak/1024:.1f} MiB")
    print(f"T_full_scan={t_scan*1000:.1f}ms rows={len(r['rows'])} last_id={last}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
