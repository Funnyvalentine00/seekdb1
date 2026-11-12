#!/usr/bin/env python3
"""Filtered TopK correctness and optional centralized timing/memory checks.

The matrix checks 0/1/10/100% equality filters, K=1/10/100, both scalar
types and operand orders, and ascending alias/direct distance ordering.
Use --benchmark only on the machine coordinating paired performance runs.
"""

import argparse
import json
import math
from pathlib import Path
import random
import resource
import tempfile
import time

from topk_memory import Session


MAX_LIMIT = 9223372036854775807


def literal(text):
    return "'" + text.replace("'", "''") + "'"


def vector_text(vector):
    return "[" + ",".join(map(str, vector)) + "]"


def check_rows(session, sql, expected, mode="exact"):
    response = session.request(sql, mode)
    actual = response["rows"]
    assert len(actual) == len(expected), (sql[:200], actual, expected)
    for row, want in zip(actual, expected):
        assert len(row) == len(want), (sql[:200], row, want)
        for value, target in zip(row, want):
            if isinstance(target, float):
                assert math.isclose(value, target, rel_tol=1e-8, abs_tol=1e-8), (sql[:200], row, want)
            else:
                assert value == target, (sql[:200], row, want)


def fixture(session, count, ncat, dim=128):
    # The PK deliberately is not the first declared column. Integer vector
    # coordinates keep every squared-distance sum exactly representable in
    # float32, so Python's integer oracle is independent of SIMD reduction.
    session.request(f"CREATE TABLE docs (cat VARCHAR(16), id BIGINT PRIMARY KEY, "
                    f"bucket BIGINT, v VECTOR({dim}));")
    rng = random.Random(17)
    rows = [(ident, ident % ncat, [rng.randrange(-5, 6) for _ in range(dim)])
            for ident in range(count, 0, -1)]
    for start in range(0, count, 100):
        values = ",".join(f"('c{category}',{ident},{category},'{vector_text(vec)}')"
                          for ident, category, vec in rows[start:start + 100])
        session.request("INSERT INTO docs VALUES " + values + ";")
    return rows


def matrix(binary, count=100, repetitions=0):
    for selectivity, ncat, target in [(0, 100, -1), (1, 100, 0), (10, 10, 0), (100, 1, 0)]:
        with tempfile.TemporaryDirectory(prefix="mvd-filter-matrix-") as directory:
            session = Session(binary, directory)
            try:
                rows = fixture(session, count, ncat)
                query = [0] * 128
                distance = f"L2_DISTANCE(v,'{vector_text(query)}')"
                ordered = sorted((sum(x * x for x in vec), ident)
                                 for ident, category, vec in rows if category == target)
                # Equal scalar values must work before any pending write too.
                session.request("BEGIN;")
                for column, value in [("cat", literal(f"c{target}")), ("bucket", str(target))]:
                    for reverse in [False, True]:
                        predicate = f"{value} = {column}" if reverse else f"{column} = {value}"
                        for k in [0, 1, 10, 100, MAX_LIMIT]:
                            expected = [[ident] for _, ident in ordered[:k]]
                            check_rows(session, f"SELECT id FROM docs WHERE {predicate} "
                                       f"ORDER BY {distance} LIMIT {k};", expected)
                            check_rows(session, f"SELECT id, {distance} AS d FROM docs WHERE {predicate} "
                                       f"ORDER BY d LIMIT {k};",
                                       [[ident, math.sqrt(squared)] for squared, ident in ordered[:k]], "ann")
                session.request("ROLLBACK;")
                if repetitions:
                    for column, value in [("cat", literal(f"c{target}")), ("bucket", str(target))]:
                        for pending in [False, True]:
                            if pending:
                                session.request("BEGIN;")
                                # A real unchanged-value write creates pending state without
                                # changing the result, exercising the generic transaction path.
                                session.request(f"UPDATE docs SET bucket = {count % ncat} WHERE id = {count};")
                            for k in [1, 10, 100]:
                                sql = (f"SELECT id FROM docs WHERE {column} = {value} "
                                       f"ORDER BY {distance} LIMIT {k};")
                                expected = [[ident] for _, ident in ordered[:k]]
                                check_rows(session, sql, expected)
                                started = time.perf_counter()
                                for _ in range(repetitions):
                                    response = session.request(sql)
                                    assert response["rows"] == expected
                                elapsed = time.perf_counter() - started
                                print(json.dumps({"rows": count, "dim": 128, "selectivity_pct": selectivity,
                                                  "column_type": "VARCHAR" if column == "cat" else "BIGINT",
                                                  "k": k, "pending": pending, "queries": repetitions,
                                                  "seconds": elapsed, "us_per_query": elapsed * 1e6 / repetitions}),
                                      flush=True)
                            if pending:
                                session.request("ROLLBACK;")
            finally:
                session.close()


def boundaries(binary):
    with tempfile.TemporaryDirectory(prefix="mvd-filter-boundaries-") as directory:
        session = Session(binary, directory)
        distance = "L2_DISTANCE(v,'[0,0]')"

        def topk(where, ids, order=None, limit=MAX_LIMIT):
            for mode in ["exact", "ann"]:
                check_rows(session, f"SELECT id FROM edge WHERE {where} "
                           f"ORDER BY {order or distance} LIMIT {limit};", [[ident] for ident in ids], mode)

        def mutations():
            session.request("UPDATE edge SET id = -8, v = '[0,0]' WHERE id = 8;")
            session.request("UPDATE edge SET cat = 'keep', bucket = 1 WHERE id = 1;")
            session.request("DELETE FROM edge WHERE id = 3;")
            session.request("INSERT INTO edge VALUES (9,'keep',1,'[1,0]');")

        def committed():
            topk("cat = 'keep'", [-8, 1, 9, 12])
            topk("1 = bucket", [-8, 1, 9, 12])
            topk("id = -8", [-8])
            topk("id = 8", [])

        try:
            session.request("CREATE TABLE edge (id BIGINT PRIMARY KEY, cat VARCHAR(8), bucket BIGINT, v VECTOR(2));")
            topk("cat = 'keep'", [])
            # Commit the larger tied PK first so a distance-only K=1 heap
            # cannot pass just because the physical slot order happens to fit.
            session.request("INSERT INTO edge VALUES (8,'keep',1,'[1,0]');")
            session.request("INSERT INTO edge VALUES (3,'keep',1,'[-1,0]'),"
                            "(1,'skip',2,'[0,0]'),(2,'skip',2,'[0,0]'),(12,'keep',1,'[4,0]');")
            for where in ["cat = 'keep'", "'keep' = cat", "bucket = 1", "1 = bucket",
                          "cat = 'keep' AND bucket = 1", "bucket < 2"]:
                topk(where, [3, 8, 12])
                topk(where, [3], limit=1)
            topk("cat = 'keep'", [12, 3, 8], distance + " DESC")
            topk("cat = 'keep'", [8, 3, 12], distance + ", id DESC")
            topk("'keep' = 'keep'", [1, 2, 3, 8, 12])
            topk("cat = 'keep'", [12, 8], "id DESC", 2)
            check_rows(session, f"SELECT id FROM edge WHERE cat = 'keep' ORDER BY {distance};",
                       [[3], [8], [12]])
            check_rows(session, "SELECT id, L2_DISTANCE(v,'[0,0]') AS d, L2_DISTANCE(v,'[1,0]') AS other "
                       "FROM edge WHERE cat = 'keep' ORDER BY d LIMIT 2;", [[3, 1.0, 2.0], [8, 1.0, 0.0]])
            session.request("BEGIN;")
            mutations()
            committed()
            session.request("ROLLBACK;")
            topk("cat = 'keep'", [3, 8, 12])
            session.request("BEGIN;")
            mutations()
            committed()
            session.request("COMMIT;")
            committed()
            session.close()
            session = Session(binary, directory)
            committed()
            session.request("DELETE FROM edge;")
            topk("cat = 'keep'", [])
            # Prefix, UTF-8 and int64 equalities must compare exact stored values.
            text_rows = [(21, "", -9223372036854775808), (20, "a", 9223372036854775807),
                         (19, "aa", 9007199254740993), (18, "中", 1), (17, "中文", 2),
                         (16, "😀", 3), (15, "a'b", 4)]
            session.request("INSERT INTO edge VALUES " + ",".join(
                f"({ident},{literal(text)},{bucket},'[1,0]')" for ident, text, bucket in text_rows) + ";")
            for ident, text, bucket in text_rows:
                for predicate in [f"cat = {literal(text)}", f"{literal(text)} = cat",
                                  f"bucket = {bucket}", f"{bucket} = bucket"]:
                    topk(predicate, [ident])
            session.request("INSERT INTO edge VALUES (30,'float',7,'[16777217,0]'),(29,'float',7,'[16777216,0]');")
            check_rows(session, "SELECT id, L2_DISTANCE(v,'[16777216,0]') AS d FROM edge "
                       "WHERE cat = 'float' ORDER BY d LIMIT 10;", [[29, 0.0], [30, 0.0]])
        finally:
            session.close()


def memory_bounds(binary):
    if not hasattr(resource, "prlimit"):
        raise RuntimeError("--memory requires Linux resource.prlimit")
    count = 150000
    with tempfile.TemporaryDirectory(prefix="mvd-filter-memory-") as directory:
        session = Session(binary, directory)
        try:
            session.request("CREATE TABLE memory (id BIGINT PRIMARY KEY, cat BIGINT, v VECTOR(1));")
            for start in range(1, count + 1, 500):
                session.request("INSERT INTO memory VALUES " + ",".join(
                    f"({ident},{ident},'[1]')" for ident in range(start, start + 500)) + ";")
        finally:
            session.close()

        def bounded_checks(expected):
            limited = Session(binary, directory)
            try:
                status = Path(f"/proc/{limited.proc.pid}/status").read_text()
                vm_kib = int(next(line.split()[1] for line in status.splitlines() if line.startswith("VmSize:")))
                _, hard = resource.prlimit(limited.proc.pid, resource.RLIMIT_AS)
                soft = vm_kib * 1024 + 1024 * 1024
                if hard != resource.RLIM_INFINITY:
                    soft = min(soft, hard)
                resource.prlimit(limited.proc.pid, resource.RLIMIT_AS, (soft, hard))
                for mode in ["exact", "ann"]:
                    for predicate, rows in [("cat = -1", []), (f"cat = {count}", expected)]:
                        check_rows(limited, f"SELECT id FROM memory WHERE {predicate} "
                                   f"ORDER BY L2_DISTANCE(v,'[0]') LIMIT {MAX_LIMIT};", rows, mode)
            finally:
                limited.close()

        bounded_checks([[count]])  # 150k live rows, zero/one actual match.
        for deletion, expected in [(f"id < {count}", [[count]]), ("id > 0", [])]:
            session = Session(binary, directory)
            try:
                session.request("DELETE FROM memory WHERE " + deletion + ";")
            finally:
                session.close()
            bounded_checks(expected)  # 150k historical slots, one/zero live rows.


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="./build/minivectordb")
    parser.add_argument("--benchmark", action="store_true")
    parser.add_argument("--n", type=int, default=10000)
    parser.add_argument("--queries", type=int, default=1000)
    parser.add_argument("--memory", action="store_true")
    args = parser.parse_args()
    binary = str(Path(args.binary).resolve())
    if args.benchmark:
        matrix(binary, args.n, args.queries)
    elif args.memory:
        memory_bounds(binary)
        print("Filtered TopK memory bounds passed")
    else:
        matrix(binary)
        boundaries(binary)
        print("Filtered TopK correctness checks passed")
