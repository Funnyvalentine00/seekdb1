#!/usr/bin/env python3
"""Check CRC-32C WAL bytes and recovery across two MiniVectorDB executables.

Both executables must use the target branch's Castagnoli format. This does not
claim compatibility with the earlier IEEE CRC32 WAL format.
"""

import argparse
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile


FRAME_MAGIC = 0x4D5644434F4D4D49
DOCS_WAL = "t_1.wal"
TAGS_WAL = "t_2.wal"


def crc32c(data):
    """Independent bitwise oracle, deliberately not the database's table/HW code."""
    crc = 0xffffffff
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82f63b78 if crc & 1 else 0)
    return crc ^ 0xffffffff


def mutation(sql, count):
    return sql, {"affected_rows": count}


SETUP = [
    ("CREATE TABLE docs (id BIGINT PRIMARY KEY, body VARCHAR(16), v VECTOR(3));", {}),
    ("CREATE TABLE tags (id BIGINT PRIMARY KEY, label VARCHAR(8));", {}),
    mutation("INSERT INTO docs VALUES (1,'first','[1,0,0]'), (2,'second','[0,2,0]'), "
             "(3,'remove','[0,0,3]'), (4,'spare','[-1,2,-3]');", 4),
    mutation("INSERT INTO tags VALUES (1,'初始');", 1),
    mutation("UPDATE docs SET body = '雪😀' WHERE id = 4;", 1),
    ("BEGIN;", {}),
    mutation("UPDATE docs SET id = -1 WHERE id = 1;", 1),
    mutation("UPDATE docs SET id = 1 WHERE id = 2;", 1),
    mutation("UPDATE docs SET id = 2, v = '[1.5,2.5,-3]' WHERE id = -1;", 1),
    mutation("DELETE FROM docs WHERE id = 3;", 1),
    mutation("INSERT INTO docs VALUES (9007199254740993,'added','[4,5,6]');", 1),
    mutation("UPDATE tags SET label = '提交' WHERE id = 1;", 1),
    ("COMMIT;", {}),
]

PREFIX_DOCS = [
    [1, "first", [1, 0, 0]],
    [2, "second", [0, 2, 0]],
    [3, "remove", [0, 0, 3]],
    [4, "雪😀", [-1, 2, -3]],
]
COMMITTED_DOCS = [
    [1, "second", [0, 2, 0]],
    [2, "first", [1.5, 2.5, -3]],
    [4, "雪😀", [-1, 2, -3]],
    [9007199254740993, "added", [4, 5, 6]],
]


def queries(docs, label):
    return [
        ("SELECT id, body, v FROM docs ORDER BY id;",
         {"columns": ["id", "body", "v"], "rows": docs}),
        ("SELECT body FROM docs WHERE id = 1;",
         {"columns": ["body"], "rows": [[docs[0][1]]]}),
        ("SELECT id, label FROM tags ORDER BY id;",
         {"columns": ["id", "label"], "rows": [[1, label]]}),
    ]


def expect(actual, expected, context):
    if actual != expected:
        raise AssertionError(f"{context}: expected {expected!r}, got {actual!r}")


def session(binary, directory, steps):
    requests = [{"id": index, "op": "execute", "sql": sql}
                for index, (sql, _) in enumerate(steps)]
    requests.append({"id": len(steps), "op": "shutdown"})
    payload = "".join(json.dumps(request) + "\n" for request in requests)
    result = subprocess.run(
        [str(binary), "--stdio", f"--data-dir={directory}"],
        input=payload, encoding="utf-8", capture_output=True, timeout=20,
    )
    context = f"{binary.name} in {directory.name}"
    expect(result.returncode, 0, f"{context}, stderr={result.stderr}")
    messages = [json.loads(line) for line in result.stdout.splitlines()]
    expect(len(messages), len(requests) + 1, f"{context}, response count")
    expect(messages[0], {"type": "ready"}, context)
    for index, (sql, expected) in enumerate(steps):
        expect(messages[index + 1], {"id": index, "ok": True, **expected},
               f"{context}, {sql}")
    expect(messages[-1], {"id": len(steps), "ok": True}, context)


def validate_wal(data, expected_counts, context):
    """Validate each complete frame with an independent CRC implementation."""
    pos = 0
    frames = []
    counts = []
    if not 0 < len(data) <= (8 << 20):
        raise AssertionError(f"{context}: WAL size {len(data)} outside replay range")
    while pos < len(data):
        if len(data) - pos < 16:
            raise AssertionError(f"{context}: short frame header or trailer at {pos}")
        # The on-disk integers use the writer's native byte order.
        length, crc = struct.unpack_from("=II", data, pos)
        payload_end = pos + 8 + length
        end = payload_end + 8
        if length < 4 or end > len(data):
            raise AssertionError(f"{context}: invalid frame length {length} at {pos}")
        payload = data[pos + 8:payload_end]
        expect(crc32c(payload), crc, f"{context}, CRC-32C at {pos}")
        expect(struct.unpack_from("=Q", data, payload_end)[0], FRAME_MAGIC,
               f"{context}, trailer at {pos}")
        counts.append(struct.unpack_from("=I", payload)[0])
        frames.append((pos, payload_end, end))
        pos = end
    expect(counts, expected_counts, f"{context}, operation counts per frame")
    return frames


def reset_rows(directory):
    # The copied catalog and row headers are intact. Removing all slots forces
    # startup to reconstruct table state from WAL rather than mapped rows.
    for path in sorted(directory.glob("*.rows")):
        with path.open("r+b") as file:
            expect(file.read(8), b"MVDROWS1", path)
            file.seek(12)
            file.write(struct.pack("=I", 0))
            file.truncate(64)


def check_replay(writer_dir, reader, destination):
    shutil.copytree(writer_dir, destination)
    reset_rows(destination)
    steps = queries(COMMITTED_DOCS, "提交")
    session(reader, destination, steps)
    session(reader, destination, steps)
    for name in (DOCS_WAL, TAGS_WAL):
        expect((destination / name).read_bytes() == (writer_dir / name).read_bytes(),
               True, f"{destination.name}, {name} changed on replay")


def damaged_tails(wal, frame):
    start, payload_end, end = frame
    # Cut inside and between the length, CRC, payload and trailer fields.
    for name, offset in (
            ("length-partial", start + 3), ("length-end", start + 4),
            ("crc-partial", start + 7), ("crc-end", start + 8),
            ("payload-first-byte", start + 9), ("payload-last-byte-missing", payload_end - 1),
            ("payload-end", payload_end), ("trailer-first-byte", payload_end + 1),
            ("trailer-last-byte-missing", end - 1)):
        yield name, wal[:offset]
    for name, offset in (
            ("crc-mismatch", start + 4), ("payload-bitflip", payload_end - 1),
            ("bad-magic", payload_end)):
        damaged = bytearray(wal)
        damaged[offset] ^= 1
        yield name, damaged
    for name, length in (("zero-length", 0), ("oversize-length", (64 << 20) + 1)):
        damaged = bytearray(wal)
        struct.pack_into("=I", damaged, start, length)
        yield name, damaged


def check_bad_tails(writer_dir, reader, root, wal, frame):
    good_prefix = wal[:frame[0]]
    count = 0
    for name, data in damaged_tails(wal, frame):
        directory = root / name
        shutil.copytree(writer_dir, directory)
        reset_rows(directory)
        path = directory / DOCS_WAL
        path.write_bytes(data)
        # Replay keeps the valid docs prefix and the intact tags log.
        session(reader, directory, queries(PREFIX_DOCS, "提交"))
        expect(path.read_bytes() == good_prefix, True, f"{name}, WAL truncation")
        session(reader, directory, [
            mutation("INSERT INTO docs VALUES (5,'after','[7,8,9]');", 1),
        ])
        appended_wal = path.read_bytes()
        expect(appended_wal.startswith(good_prefix), True, f"{name}, append prefix")
        validate_wal(appended_wal, [4, 1, 1], name)
        reset_rows(directory)
        session(reader, directory, queries(PREFIX_DOCS + [[5, "after", [7, 8, 9]]], "提交"))
        count += 1
    return count


def run(baseline, candidate):
    expect(crc32c(b"123456789"), 0xe3069283, "CRC-32C known vector")
    expect(crc32c(b""), 0, "CRC-32C empty input")
    with tempfile.TemporaryDirectory(prefix="mvd-wal-") as temp:
        root = Path(temp)
        baseline_dir, candidate_dir = root / "baseline", root / "candidate"
        logs = {}
        for binary, directory in ((baseline, baseline_dir), (candidate, candidate_dir)):
            directory.mkdir()
            session(binary, directory, SETUP + queries(COMMITTED_DOCS, "提交"))
            expect(sorted(p.name for p in directory.glob("*.wal")),
                   [DOCS_WAL, TAGS_WAL], directory.name)
            logs[directory] = {name: (directory / name).read_bytes()
                               for name in (DOCS_WAL, TAGS_WAL)}
        baseline_logs, candidate_logs = logs[baseline_dir], logs[candidate_dir]
        frames = {}
        for name, counts in ((DOCS_WAL, [4, 1, 4]), (TAGS_WAL, [1, 1])):
            frames[name] = validate_wal(baseline_logs[name], counts, f"baseline/{name}")
            validate_wal(candidate_logs[name], counts, f"candidate/{name}")
            expect(baseline_logs[name] == candidate_logs[name], True,
                   f"{name}, WAL bytes differ between executables")
        print("WAL bytes match across both tables; 5 frame CRCs and trailers checked per writer")

        check_replay(baseline_dir, candidate, root / "baseline-to-candidate")
        check_replay(candidate_dir, baseline, root / "candidate-to-baseline")
        print("Both replay directions and subsequent clean restarts match the expected rows")

        variants = check_bad_tails(
            baseline_dir, candidate, root, baseline_logs[DOCS_WAL], frames[DOCS_WAL][-1],
        )
        print(f"{variants} damaged WAL tails rejected; valid prefix and appended rows recovered")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    args = parser.parse_args()
    run(args.baseline.resolve(), args.candidate.resolve())
