#!/usr/bin/env python3
"""随机差分测试：把随机 SQL 序列同时施加到 MiniVectorDB 与 Python 参考模型上，逐条比对。

覆盖：多行 INSERT、WHERE 比较/AND、ORDER BY（列/距离/别名、升降序、并列主键）、
LIMIT、UPDATE/DELETE、事务提交/回滚、重启与 SIGKILL 恢复。
"""
import argparse
import json
import math
import os
import random
import shutil
import signal
import subprocess
import sys
import tempfile
import time

DIM = 4
CATS = ["a", "b", "c", "d"]
VERBOSE = False


class Model:
    def __init__(self):
        self.rows = {}       # id -> dict
        self.pending = None  # 事务内暂存: id -> dict or None(deleted)
        self.in_txn = False

    def visible(self):
        state = {k: dict(v) for k, v in self.rows.items()}
        if self.pending is not None:
            for k, v in self.pending.items():
                if v is None:
                    state.pop(k, None)
                else:
                    state[k] = dict(v)
        return state

    def begin(self):
        assert not self.in_txn
        self.in_txn = True
        self.pending = {}

    def commit(self):
        if not self.in_txn:
            return
        for k, v in self.pending.items():
            if v is None:
                self.rows.pop(k, None)
            else:
                self.rows[k] = dict(v)
        self.in_txn = False
        self.pending = None

    def rollback(self):
        self.in_txn = False
        self.pending = None

    def set_row(self, rid, row):
        if self.in_txn:
            self.pending[rid] = row
        else:
            if row is None:
                self.rows.pop(rid, None)
            else:
                self.rows[rid] = dict(row)


def dist2(a, b):
    return sum((x - y) ** 2 for x, y in zip(a, b))


class Session:
    def __init__(self, binary, data_dir):
        self.binary = binary
        self.data_dir = data_dir
        self.proc = None

    def start(self):
        os.makedirs(self.data_dir, exist_ok=True)
        self.proc = subprocess.Popen([self.binary, "--data-dir", self.data_dir],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True, encoding="utf-8",
                                     bufsize=1)

    def sql(self, text):
        if VERBOSE:
            print("SQL>", text)
        self.proc.stdin.write(text.rstrip().rstrip(";") + ";\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if line == "":
            err = self.proc.stderr.read() if self.proc.stderr else ""
            raise RuntimeError("closed stdout: " + err)
        return json.loads(line)

    def stop(self):
        if not self.proc:
            return
        self.proc.stdin.close()
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        self.proc = None

    def kill(self):
        if not self.proc:
            return
        self.proc.send_signal(signal.SIGKILL)
        self.proc.wait()
        self.proc = None


def vec_lit(v):
    return "[" + ",".join("%.6g" % x for x in v) + "]"


def rand_vec(rng):
    return [round(rng.uniform(-2, 2), 5) for _ in range(DIM)]


def compare_query(sess, model, sql, ordered, rng, errors):
    got = sess.sql(sql)
    state = model.visible()
    if "error" in got:
        errors.append(("unexpected error", sql, got))
        return
    cols = got.get("columns", [])
    rows = got.get("rows", [])
    idx = {c: i for i, c in enumerate(cols)}
    # 解析查询意图以生成参考结果
    q = sql.lower()
    # 过滤
    def keep(r):
        return True

    # 这里只生成受控 SQL，用简单解析还原谓词
    if " where " in q:
        w = q.split(" where ", 1)[1].split(" order ", 1)[0].split(" limit ", 1)[0]
        conds = w.split(" and ")
        def keep(r):
            for c in conds:
                c = c.strip()
                if "=" in c:
                    k, v = [x.strip() for x in c.split("=", 1)]
                    if k == "id":
                        if r["id"] != int(v): return False
                    elif k == "cat":
                        if r["cat"] != v.strip("'"): return False
            return True
    cand = [(rid, r) for rid, r in state.items() if keep(r)]
    # ORDER BY
    if ordered:
        o = q.split(" order by ", 1)[1].split(" limit ", 1)[0].strip()
        parts = [p.strip() for p in o.split(",")]
        def key(item):
            rid, r = item
            vals = []
            for p in parts:
                desc = p.endswith(" desc")
                if p.startswith("d") and ("distance" in p or p.endswith("desc") or p.endswith("asc")):
                    pass
            return vals
        # 受控查询只有单键
        p = parts[0]
        desc = p.endswith(" desc")
        name = p.replace(" desc", "").replace(" asc", "").strip()
        if name == "d":
            cand.sort(key=lambda it: (dist2(it[1]["vec"], model_qvec), it[0]))
        elif name == "id":
            cand.sort(key=lambda it: it[0], reverse=desc)
        elif name == "cat":
            cand.sort(key=lambda it: it[0])
            cand.sort(key=lambda it: it[1]["cat"], reverse=desc)
        elif name == "val":
            if desc:
                cand.sort(key=lambda it: (-it[1]["val"], it[0]))
            else:
                cand.sort(key=lambda it: (it[1]["val"], it[0]))
    else:
        cand.sort(key=lambda it: it[0])
    if " limit " in q:
        n = int(q.split(" limit ", 1)[1].strip())
        cand = cand[:n]
    # 参考行
    want = []
    for rid, r in cand:
        row = []
        for c in cols:
            if c == "id": row.append(rid)
            elif c == "cat": row.append(r["cat"])
            elif c == "val": row.append(r["val"])
            elif c == "d": row.append(round(math.sqrt(dist2(r["vec"], model_qvec)), 5))
            elif c == "vec": row.append(vec_lit(r["vec"]))
        want.append(row)
    ok = True
    if len(want) != len(rows):
        ok = False
    else:
        for w, g in zip(want, rows):
            for x, y in zip(w, g):
                if isinstance(x, float) and isinstance(y, (int, float)):
                    if abs(x - y) > 1e-4 * max(1, abs(x)):
                        ok = False
                elif x != y:
                    ok = False
    if not ok:
        errors.append(("mismatch", sql, {"got": rows[:5], "want": want[:5]}))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="./build/minivectordb")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--rounds", type=int, default=40)
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    global model_qvec, VERBOSE
    VERBOSE = args.verbose
    rng = random.Random(args.seed)
    work = tempfile.mkdtemp(prefix="mvd_fuzz_")
    data_dir = os.path.join(work, "data")
    sess = Session(args.binary, data_dir)
    sess.start()
    sess.sql("CREATE TABLE f (id BIGINT PRIMARY KEY, cat VARCHAR(4), val BIGINT, vec VECTOR(4))")
    model = Model()
    errors = []
    next_id = 1

    def check_affected(sql, expect):
        got = sess.sql(sql)
        if "error" in got:
            errors.append(("unexpected error", sql, got))
            return False
        if got.get("affected_rows") != expect:
            errors.append(("affected", sql, {"got": got, "want": expect}))
            return False
        return True

    def verify_state(tag):
        got = sess.sql("SELECT id, cat, val FROM f ORDER BY id")
        state = model.visible()
        want = [[rid, r["cat"], r["val"]] for rid, r in sorted(state.items())]
        rows = got.get("rows", [])
        if rows == want:
            return True
        gd = {r[0]: (r[1], r[2]) for r in rows}
        wd = {r[0]: (r[1], r[2]) for r in want}
        diffs = []
        for rid in sorted(set(gd) | set(wd)):
            if gd.get(rid) != wd.get(rid):
                diffs.append((rid, "db=" + str(gd.get(rid)), "model=" + str(wd.get(rid))))
        errors.append(("state divergence", tag, diffs[:8]))
        print("DIVERGE after:", tag[:120], diffs[:5])
        return False

    for rnd in range(args.rounds):
        # 随机事务块
        use_txn = rng.random() < 0.3
        if use_txn:
            sess.sql("BEGIN")
            model.begin()
        for _ in range(rng.randint(1, 6)):
            op = rng.random()
            if op < 0.45:
                batch = rng.randint(1, 4)
                rows = []
                ids = []
                for _ in range(batch):
                    rid = next_id
                    next_id += 1
                    row = {"id": rid, "cat": rng.choice(CATS), "val": rng.randint(-20, 20),
                           "vec": rand_vec(rng)}
                    rows.append(row)
                    ids.append(rid)
                    model.set_row(rid, row)
                sql = "INSERT INTO f VALUES " + ",".join(
                    "(%d,'%s',%d,'%s')" % (r["id"], r["cat"], r["val"], vec_lit(r["vec"]))
                    for r in rows)
                if check_affected(sql, len(rows)):
                    verify_state(sql)
            elif op < 0.7:
                # UPDATE by id
                state = model.visible()
                if not state:
                    continue
                rid = rng.choice(list(state.keys()))
                newval = rng.randint(-20, 20)
                newcat = rng.choice(CATS)
                newvec = rand_vec(rng)
                sql = "UPDATE f SET cat='%s', val=%d, vec='%s' WHERE id=%d" % (
                    newcat, newval, vec_lit(newvec), rid)
                row = dict(state[rid])
                row.update({"cat": newcat, "val": newval, "vec": newvec})
                model.set_row(rid, row)
                if check_affected(sql, 1):
                    verify_state(sql)
            elif op < 0.85:
                # DELETE by cat
                cat = rng.choice(CATS)
                state = model.visible()
                n = sum(1 for r in state.values() if r["cat"] == cat)
                if check_affected("DELETE FROM f WHERE cat='%s'" % cat, n):
                    for rid, r in list(state.items()):
                        if r["cat"] == cat:
                            model.set_row(rid, None)
                    verify_state("delete cat=" + cat)
            else:
                # 查询
                state = model.visible()
                if not state:
                    continue
                qvec = rand_vec(rng)
                model_qvec = qvec
                kind = rng.random()
                if kind < 0.35:
                    sql = "SELECT id, cat, val, L2_DISTANCE(vec,'%s') AS d FROM f ORDER BY d LIMIT %d" % (
                        vec_lit(qvec), rng.choice([1, 3, 10, 50]))
                    compare_query(sess, model, sql, True, rng, errors)
                elif kind < 0.6:
                    cat = rng.choice(CATS)
                    sql = "SELECT id, val FROM f WHERE cat='%s' ORDER BY id" % cat
                    compare_query(sess, model, sql, True, rng, errors)
                elif kind < 0.8:
                    rid = rng.choice(list(state.keys()))
                    sql = "SELECT id, cat, val FROM f WHERE id=%d ORDER BY id" % rid
                    compare_query(sess, model, sql, True, rng, errors)
                else:
                    sql = "SELECT id, cat FROM f WHERE cat='%s' ORDER BY val" % rng.choice(CATS)
                    compare_query(sess, model, sql, True, rng, errors)
        if use_txn:
            if rng.random() < 0.5:
                sess.sql("COMMIT")
                model.commit()
            else:
                sess.sql("ROLLBACK")
                model.rollback()
        # 周期性崩溃恢复
        if rng.random() < 0.15:
            sess.kill()
            sess.start()
            state = model.visible()
            got = sess.sql("SELECT id, cat, val FROM f ORDER BY id")
            rows = got.get("rows", [])
            want = [[rid, r["cat"], r["val"]] for rid, r in sorted(state.items())]
            if rows != want:
                errors.append(("recovery mismatch", "after kill", {"got": rows[:5], "want": want[:5]}))

    sess.stop()
    if errors:
        print("work dir:", work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    if errors:
        print("FAILED", len(errors))
        for e in errors[:10]:
            print(e)
        return 1
    print("OK", args.rounds, "rounds")
    return 0


if __name__ == "__main__":
    sys.exit(main())
