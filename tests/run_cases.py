#!/usr/bin/env python3
"""MiniVectorDB 本地用例驱动。

按公开题包语义执行 .test 中的 SQL，并逐条比对 .result 中的 JSON 断言：
- 每条 SQL 一行，程序回一行 JSON；
- {} 表示成功；
- affected_rows 必须精确一致；
- columns/rows 必须一致（unordered 时按多重集合比较）；
- 浮点使用绝对/相对 1e-5 容差。

额外支持指令行（以 # 开头，不会发给数据库）：
    # restart [--flag ...]   正常关闭并以同一数据目录重启
    # kill [--flag ...]      SIGKILL 后以同一数据目录重启
    # sleep MS               等待
    # expect-error           下一条语句必须返回 error
"""
import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

FLOAT_TOL_ABS = 1e-5
FLOAT_TOL_REL = 1e-5


def close_quote(s):
    single = double = False
    i = 0
    while i < len(s):
        c = s[i]
        if single:
            if c == "'":
                if i + 1 < len(s) and s[i + 1] == "'":
                    i += 1
                else:
                    single = False
        elif double:
            if c == '"':
                double = False
        else:
            if c == "'":
                single = True
            elif c == '"':
                double = True
        i += 1
    return not single and not double


def split_statements(text):
    """把一段文本按顶层分号拆成语句（忽略引号内的分号）。"""
    out, buf, i = [], [], 0
    while i < len(text):
        c = text[i]
        if c == "'":
            j = i + 1
            buf.append(c)
            while j < len(text):
                buf.append(text[j])
                if text[j] == "'":
                    if j + 1 < len(text) and text[j + 1] == "'":
                        buf.append(text[j + 1])
                        j += 2
                        continue
                    break
                j += 1
            i = j + 1
            continue
        if c == ';':
            out.append(''.join(buf))
            buf = []
        else:
            buf.append(c)
        i += 1
    if ''.join(buf).strip():
        out.append(''.join(buf))
    return out


class Session:
    def __init__(self, binary, data_dir, extra_args=None):
        self.binary = binary
        self.data_dir = data_dir
        self.extra_args = extra_args or []
        self.proc = None

    def start(self):
        os.makedirs(self.data_dir, exist_ok=True)
        self.proc = subprocess.Popen(
            [self.binary, "--data-dir", self.data_dir] + self.extra_args,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", bufsize=1)

    def query(self, sql, timeout=30.0):
        assert self.proc is not None
        self.proc.stdin.write(sql.strip() + ";\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if line == "":
            err = self.proc.stderr.read() if self.proc.stderr else ""
            raise RuntimeError("database closed stdout. stderr:\n" + err)
        return line.strip()

    def stop(self):
        if self.proc is None:
            return
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        self.proc = None

    def kill(self):
        if self.proc is None:
            return
        self.proc.send_signal(signal.SIGKILL)
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass
        self.proc = None


def is_float(x):
    return isinstance(x, float)


def cell_equal(expected, actual):
    if isinstance(expected, (int, float)) and not isinstance(expected, bool):
        if isinstance(actual, str):
            try:
                actual = float(actual)
            except ValueError:
                return False
        if not isinstance(actual, (int, float)):
            return False
        if isinstance(expected, int) and isinstance(actual, int):
            return expected == actual
        e, a = float(expected), float(actual)
        if abs(e - a) <= FLOAT_TOL_ABS:
            return True
        return abs(e - a) <= FLOAT_TOL_REL * max(1.0, abs(e), abs(a))
    if isinstance(expected, str) and isinstance(actual, str):
        return expected == actual
    return expected == actual


def row_equal(expected, actual):
    if len(expected) != len(actual):
        return False
    return all(cell_equal(e, a) for e, a in zip(expected, actual))


def check(expect, resp):
    if not isinstance(resp, dict):
        return False, "response is not a JSON object"
    if "error" in expect:
        if "error" not in resp:
            return False, f"expected error, got {resp}"
        return True, ""
    if "error" in resp:
        return False, f"unexpected error: {resp}"
    if "affected_rows" in expect:
        if resp.get("affected_rows") != expect["affected_rows"]:
            return False, f"affected_rows {resp.get('affected_rows')} != {expect['affected_rows']}"
        return True, ""
    if "columns" in expect:
        if resp.get("columns") != expect["columns"]:
            return False, f"columns {resp.get('columns')} != {expect['columns']}"
        got = resp.get("rows", [])
        want = expect.get("rows", [])
        if expect.get("unordered"):
            pool = list(got)
            for w in want:
                for idx, g in enumerate(pool):
                    if row_equal(w, g):
                        pool.pop(idx)
                        break
                else:
                    return False, f"missing row {w} in {got}"
            return True, ""
        if len(got) != len(want):
            return False, f"row count {len(got)} != {len(want)}"
        for i, (w, g) in enumerate(zip(want, got)):
            if not row_equal(w, g):
                return False, f"row {i}: {g} != {w}"
        return True, ""
    return True, ""


def run_case(binary, test_path, result_path, work_dir, verbose=False):
    with open(test_path, encoding="utf-8") as f:
        lines = f.readlines()
    with open(result_path, encoding="utf-8") as f:
        expected = json.load(f)

    data_dir = os.path.join(work_dir, "data")
    if os.path.isdir(data_dir):
        shutil.rmtree(data_dir)
    os.makedirs(data_dir)
    sess = Session(binary, data_dir)
    sess.start()

    stmt_index = 0
    pending_args = []
    failures = []
    expect_error_next = False
    try:
        for raw in lines:
            line = raw.strip()
            if not line:
                continue
            if line.startswith("#"):
                body = line[1:].strip()
                parts = body.split()
                if not parts:
                    continue
                cmd = parts[0]
                if cmd == "restart":
                    pending_args = parts[1:]
                    sess.stop()
                    sess.extra_args = pending_args
                    sess.start()
                elif cmd == "kill":
                    pending_args = parts[1:]
                    sess.kill()
                    sess.extra_args = pending_args
                    sess.start()
                elif cmd == "sleep":
                    time.sleep(int(parts[1]) / 1000.0)
                elif cmd == "expect-error":
                    expect_error_next = True
                continue
            for sql in split_statements(line):
                if not sql.strip():
                    continue
                if stmt_index >= len(expected):
                    failures.append(f"extra statement {stmt_index}: {sql.strip()[:80]}")
                    stmt_index += 1
                    continue
                try:
                    resp = json.loads(sess.query(sql))
                except Exception as e:
                    failures.append(f"statement {stmt_index} failed to run: {e}")
                    stmt_index += 1
                    continue
                expect = {"error": True} if expect_error_next else expected[stmt_index]
                expect_error_next = False
                ok, msg = check(expect, resp)
                if not ok:
                    failures.append(f"statement {stmt_index}: {sql.strip()[:100]}\n  {msg}")
                if verbose:
                    print(f"[{stmt_index}] {sql.strip()[:80]} -> {resp}")
                stmt_index += 1
    finally:
        sess.stop()

    if stmt_index != len(expected):
        failures.append(f"statement count {stmt_index} != expected {len(expected)}")
    return failures


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cases_dir", nargs="?", default="mysqltest/cases")
    ap.add_argument("--binary", default="./build/minivectordb")
    ap.add_argument("--case", default=None)
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    cases = sorted(os.listdir(args.cases_dir))
    tests = [c for c in cases if c.endswith(".test")]
    if args.case:
        tests = [args.case + ".test"]
    total = 0
    failed = 0
    for t in tests:
        name = t[:-5]
        test_path = os.path.join(args.cases_dir, t)
        result_path = os.path.join(args.cases_dir, name + ".result")
        if not os.path.exists(result_path):
            continue
        work = tempfile.mkdtemp(prefix=f"mvd_{name}_")
        failures = run_case(args.binary, test_path, result_path, work, args.verbose)
        total += 1
        if failures:
            failed += 1
            print(f"FAIL {name}")
            for f in failures:
                print("   " + f)
        else:
            print(f"PASS {name}")
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)
    print(f"\n{total - failed}/{total} cases passed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
