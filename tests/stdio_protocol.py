#!/usr/bin/env python3
"""Check JSON Lines framing, request IDs, and SQL result encoding."""

import argparse
import json
import subprocess
import tempfile


def reject_constant(value):
    raise AssertionError(f"non-finite JSON number: {value}")


def run(binary):
    large_id = 10**30 + 7
    bigint = 9223372036854775807
    content = '雪😀; "quoted"\n\\path'
    text_id = 'query "雪"\n😀'
    requests = [
        {"id": large_id, "op": "execute", "sql":
         "CREATE TABLE docs (id BIGINT PRIMARY KEY, content VARCHAR(30), v VECTOR(2));"},
        {"id": True, "op": "execute", "search_mode": "ann", "sql":
         f"INSERT INTO docs VALUES ({bigint}, '{content}', '[1.5,-2]');"},
        {"id": 2.0, "op": "execute", "search_mode": "invalid", "sql": "BEGIN;"},
        {"id": 3, "op": "execute", "sql": "SELECT 雪 FROM docs;"},
        {"id": text_id, "op": "execute", "search_mode": "exact", "sql":
         "SELECT id, content, v FROM docs;"},
        {"id": {"request": [4, None]}, "op": "execute", "sql": "SELECT id FROM docs WHERE id = 0;"},
        {"id": 5, "op": "execute", "sql": "BEGIN;"},
        {"id": 6, "op": "execute", "sql": "DELETE FROM docs;"},
        {"id": None, "op": "shutdown"},
    ]
    payload = "\n".join(json.dumps(request, ensure_ascii=index != 3)
                        for index, request in enumerate(requests)) + "\n"
    with tempfile.TemporaryDirectory() as directory:
        command = [binary, "--stdio", f"--data-dir={directory}"]
        output = subprocess.run(command, input=payload.encode(), capture_output=True, timeout=10)
        assert output.stdout, (output.returncode, output.stderr.decode())
        assert output.returncode == 0, output.stderr.decode()
        assert output.stdout.endswith(b"\n"), output.stdout
        lines = output.stdout.decode().split("\n")[:-1]
        assert len(lines) == 1 + len(requests), lines
        ready, *responses = [json.loads(line, parse_constant=reject_constant) for line in lines]
        assert ready == {"type": "ready"}, ready
        for request, response in zip(requests, responses):
            assert type(response["id"]) is type(request["id"]), response
            assert response["id"] == request["id"], response
            assert type(response["ok"]) is bool, response
        assert responses[0] == {"id": large_id, "ok": True}, responses[0]
        assert responses[1] == {"id": True, "ok": True, "affected_rows": 1}, responses[1]
        for response, code in zip(responses[2:4], ["invalid_argument", "syntax_error"]):
            assert response["ok"] is False, response
            assert response["error"]["code"] == code, response
            assert response["error"]["message"], response
        assert responses[4] == {"id": text_id, "ok": True,
                                "columns": ["id", "content", "v"],
                                "rows": [[bigint, content, [1.5, -2]]]}, responses[4]
        assert type(responses[4]["rows"][0][0]) is int, responses[4]
        assert responses[5] == {"id": {"request": [4, None]}, "ok": True,
                                "columns": ["id"], "rows": []}, responses[5]
        assert responses[6] == {"id": 5, "ok": True}, responses[6]
        assert responses[7] == {"id": 6, "ok": True, "affected_rows": 1}, responses[7]
        assert responses[8] == {"id": None, "ok": True}, responses[8]

        reopened = subprocess.run(command, input=b'{"id":7,"op":"execute",'
                                 b'"sql":"SELECT id FROM docs;"}\n{"id":8,"op":"shutdown"}\n',
                                 check=True, capture_output=True, timeout=10)
        assert [json.loads(line) for line in reopened.stdout.splitlines()] == [
            {"type": "ready"}, {"id": 7, "ok": True, "columns": ["id"], "rows": [[bigint]]},
            {"id": 8, "ok": True},
        ], reopened.stdout


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="./build/minivectordb")
    run(parser.parse_args().binary)
    print("JSON Lines protocol checks passed")
