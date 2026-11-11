#!/usr/bin/env python3
"""SQL comparison and TopK LIMIT regressions through the JSON Lines interface."""

import argparse
import json
import operator
from pathlib import Path
import subprocess
import tempfile
import unittest


COMPARISONS = [("=", operator.eq), ("<", operator.lt), ("<=", operator.le),
               (">", operator.gt), (">=", operator.ge)]


class QueryBoundaries(unittest.TestCase):
    binary = "./build/minivectordb"

    def check_session(self, directory, steps, mode="exact"):
        requests = [{"id": i, "op": "execute", "sql": sql, "search_mode": mode}
                    for i, (sql, _) in enumerate(steps)]
        requests.append({"id": len(steps), "op": "shutdown"})
        result = subprocess.run(
            [self.binary, "--stdio", "--data-dir=" + directory],
            input="".join(json.dumps(r) + "\n" for r in requests),
            capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        messages = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(messages[0], {"type": "ready"})
        self.assertEqual(len(messages), len(requests) + 1)
        for ident, (sql, expected) in enumerate(steps):
            self.assertEqual(messages[ident + 1],
                             {"id": ident, "ok": True, **expected}, sql)
        self.assertEqual(messages[-1], {"id": len(steps), "ok": True})

    @staticmethod
    def selected(sql, ids):
        return sql, {"columns": ["id"], "rows": [[ident] for ident in ids]}

    def string_checks(self, rows):
        checks = []
        for literal in ["", "a", "ab", "abc", "z", "中", "中文", "😀"]:
            for symbol, compare in COMPARISONS:
                checks.append(self.selected(
                    f"SELECT id FROM strings WHERE '{literal}' {symbol} s ORDER BY id;",
                    [ident for ident, value in rows if compare(literal, value)]))
                checks.append(self.selected(
                    f"SELECT id FROM strings WHERE s {symbol} '{literal}' ORDER BY id;",
                    [ident for ident, value in rows if compare(value, literal)]))
        return checks

    def test_reversed_string_prefix_comparisons(self):
        rows = [(1, ""), (2, "a"), (3, "ab"), (4, "abc"), (5, "ac"),
                (6, "中"), (7, "中文"), (8, "😀"), (9, "😀a")]
        setup = [
            ("CREATE TABLE strings (id BIGINT PRIMARY KEY, s VARCHAR(8));", {}),
            ("INSERT INTO strings VALUES " + ",".join(
                f"({ident},'{value}')" for ident, value in rows) + ";",
             {"affected_rows": len(rows)}),
        ]
        pending = rows[:2] + [(3, "abcd")] + rows[3:] + [(10, "aa")]
        with tempfile.TemporaryDirectory(prefix="mvd-strings-") as directory:
            self.check_session(directory, setup + self.string_checks(rows) + [
                ("BEGIN;", {}),
                ("UPDATE strings SET s = 'abcd' WHERE id = 3;", {"affected_rows": 1}),
                ("INSERT INTO strings VALUES (10, 'aa');", {"affected_rows": 1}),
            ] + self.string_checks(pending) + [("ROLLBACK;", {})]
                + self.string_checks(rows))
            self.check_session(directory, self.string_checks(rows))

    def bigint_checks(self, rows):
        checks = []
        for symbol, compare in COMPARISONS:
            checks.append(self.selected(
                f"SELECT id FROM ints WHERE a {symbol} b ORDER BY id;",
                [ident for ident, a, b in rows if compare(a, b)]))
            checks.append(self.selected(
                f"SELECT id FROM ints WHERE b {symbol} a ORDER BY id;",
                [ident for ident, a, b in rows if compare(b, a)]))
            # Constants reach the generic comparison path too.
            for a, b in [(9007199254740992, 9007199254740993),
                         (-9223372036854775808, -9223372036854775807)]:
                checks.append(self.selected(
                    f"SELECT id FROM ints WHERE {a} {symbol} {b} ORDER BY id;",
                    [ident for ident, _, _ in rows] if compare(a, b) else []))
        return checks

    def test_bigint_comparisons_preserve_integer_precision(self):
        rows = [(1, 9007199254740992, 9007199254740993),
                (2, 9223372036854775806, 9223372036854775807),
                (3, -9223372036854775808, -9223372036854775807),
                (4, -9007199254740992, -9007199254740993),
                (5, 9223372036854775807, 9223372036854775807),
                (6, 0, 0)]
        setup = [
            ("CREATE TABLE ints (id BIGINT PRIMARY KEY, a BIGINT, b BIGINT);", {}),
            ("INSERT INTO ints VALUES " + ",".join(
                f"({ident},{a},{b})" for ident, a, b in rows) + ";",
             {"affected_rows": len(rows)}),
        ]
        pending = [(1, 9007199254740994, 9007199254740993)] + rows[1:]
        with tempfile.TemporaryDirectory(prefix="mvd-bigint-") as directory:
            self.check_session(directory, setup + self.bigint_checks(rows) + [
                ("BEGIN;", {}),
                ("UPDATE ints SET a = 9007199254740994 WHERE id = 1;", {"affected_rows": 1}),
            ] + self.bigint_checks(pending) + [("ROLLBACK;", {})]
                + self.bigint_checks(rows))
            self.check_session(directory, self.bigint_checks(rows))

    def topk_checks(self, ordered_ids):
        checks = []
        for limit in [0, 1, 2, 10, 2147483647, 2147483648,
                      4294967296, 9223372036854775807]:
            for where, ids in [("", ordered_ids), ("WHERE id > 0", ordered_ids),
                               ("WHERE id < 0", [])]:
                checks.append(self.selected(
                    f"SELECT id FROM vectors {where} ORDER BY L2_DISTANCE(v, '[0]') LIMIT {limit};",
                    ids[:limit]))
        return checks

    def test_large_topk_limit_is_bounded_by_candidates(self):
        for mode in ["exact", "ann"]:
            with self.subTest(mode=mode), tempfile.TemporaryDirectory(prefix="mvd-limit-") as directory:
                self.check_session(directory, [
                    ("CREATE TABLE vectors (id BIGINT PRIMARY KEY, v VECTOR(1));", {}),
                ] + self.topk_checks([]) + [
                    ("INSERT INTO vectors VALUES (1,'[3]'),(2,'[-1]'),(3,'[1]');",
                     {"affected_rows": 3}),
                ] + self.topk_checks([2, 3, 1]) + [
                    ("BEGIN;", {}),
                    ("UPDATE vectors SET v = '[0]' WHERE id = 1;", {"affected_rows": 1}),
                    ("INSERT INTO vectors VALUES (4,'[1]');", {"affected_rows": 1}),
                    ("DELETE FROM vectors WHERE id = 3;", {"affected_rows": 1}),
                ] + self.topk_checks([1, 2, 4]) + [("COMMIT;", {})]
                    + self.topk_checks([1, 2, 4]), mode)
                self.check_session(directory, self.topk_checks([1, 2, 4]) + [
                    ("DELETE FROM vectors;", {"affected_rows": 3}),
                ] + self.topk_checks([]), mode)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="./build/minivectordb")
    args, remaining = parser.parse_known_args()
    QueryBoundaries.binary = str(Path(args.binary).resolve())
    unittest.main(argv=[__file__, *remaining])
