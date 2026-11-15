#!/usr/bin/env python3
"""Run isolated BEng SQL controls. Local timings are not performance evidence."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time


NOTICE = (
    "本地构建与运行结果仅作为编译、启动和功能跑通的正确性辅助证明。"
    "本地耗时、空间占用及其变化倍数不能作为准确性能数据，"
    "不能据此宣称提速、内存收益或推算官方评分；性能仍需评测环境验证。"
)
DATABASE = "fts_beng_smoke"
GATES = ("SEEKDB_BENG_SCOPED_OVERLAP_EXPERIMENT", "SEEKDB_IK_SCOPED_OVERLAP_EXPERIMENT",
         "SEEKDB_IK_SCOPED_RETRY_PROBE")


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")


def sql(mysql, runtime, text, timeout=90):
    command = [str(mysql), "--no-defaults", "--protocol=SOCKET", "--socket=run/sql.sock",
               "--user=root", "--batch", "--raw", "--skip-column-names",
               "--default-character-set=utf8mb4", "--connect-timeout=2"]
    result = subprocess.run(command, input=text, text=True, cwd=runtime,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    check(result.returncode == 0, f"SQL failed ({result.returncode}): {result.stderr}")
    return result.stdout.strip().splitlines() if result.stdout.strip() else []


def collect_ddl(runtime, output):
    # These timestamps establish task overlap only, not a speedup measurement.
    lines = []
    for path in sorted((runtime / "log").glob("*.log*")):
        if not path.is_file() or path.is_symlink() or path.name.endswith(".wf"):
            continue
        with path.open(errors="replace") as stream:
            for line in stream:
                if ("ob_index_build_task.cpp" in line and
                    ("execute sql(" in line or "build index sstable finish(" in line)):
                    lines.append(line.rstrip())
    (output / "ddl-events.log").write_text("\n".join(lines) + "\n")
    starts, finishes = {}, {}
    for line in lines:
        if f"INTO `{DATABASE}`." not in line:
            continue
        def capture(pattern):
            match = re.search(pattern, line)
            check(match is not None, f"Missing DDL field {pattern}: {line[:200]}")
            return match.group(1)
        task = int(capture(r"opt_param\('ddl_task_id', (\d+)\)"))
        execution = int(capture(r"opt_param\('ddl_execution_id', (\d+)\)"))
        row = {
            "task": task, "execution": execution,
            "target": capture(rf"INTO `{DATABASE}`\.`([^`]+)`"),
            "snapshot": int(capture(r"as of snapshot (\d+)")),
            "worker": capture(r"\[(DdlBuild|IkDdlPair)\]"),
            "event_us": int(capture(r"event_ts_:(\d+)")),
        }
        bucket = starts if "execute sql(" in line else finishes
        check((task, execution) not in bucket, "Duplicate task/execution event")
        if bucket is finishes:
            row["ret"] = int(capture(r"build index sstable finish\(ret=(-?\d+)"))
        bucket[(task, execution)] = row
    check(starts and starts.keys() == finishes.keys(),
          f"Incomplete DDL evidence: missing finishes={starts.keys() - finishes.keys()}, "
          f"missing starts={finishes.keys() - starts.keys()}")
    jobs = []
    for key, start in starts.items():
        finish = finishes[key]
        check(all(start[k] == finish[k] for k in ("target", "snapshot", "worker")),
              "DDL identity changed between begin/end")
        check(finish["ret"] == 0 and finish["event_us"] >= start["event_us"],
              "DDL failed or has invalid event ordering")
        jobs.append(dict(start, finish_us=finish["event_us"]))
    pairs = {}
    for index in ("fti_beng", "fti_content"):
        pair = [job for job in jobs if job["target"].endswith("_" + index) or
                job["target"].endswith("_" + index + "_fts_doc_word")]
        check(len(pair) == 2, f"Expected one inverted/doc-word pair for {index}")
        pairs[index] = {
            "workers": sorted(job["worker"] for job in pair),
            "overlap_observed": max(job["event_us"] for job in pair) <
                                min(job["finish_us"] for job in pair),
        }
    evidence = {"jobs": jobs, "pairs": pairs, "notice": NOTICE}
    save(output / "ddl-evidence.json", evidence)
    return evidence


def workload(mysql, runtime, count):
    def run(text):
        return sql(mysql, runtime, f"USE {DATABASE};\n" + text)
    sql(mysql, runtime, f"CREATE DATABASE {DATABASE};")
    run("CREATE TABLE docs(id INT PRIMARY KEY, title VARCHAR(128), content TEXT);")
    terms = ["quartz quartz cobalt", "quartz amber", "cobalt zircon", "amber xenon"]
    filler = " database indexing memory transaction engine storage search " * 24
    for first in range(1, count + 1, 64):
        rows = [f"({i},'nebula report {i}','{terms[i % 4]}{filler}')"
                for i in range(first, min(first + 64, count + 1))]
        run("INSERT INTO docs VALUES " + ",".join(rows) + ";")
    run("ALTER TABLE docs ADD FULLTEXT INDEX fti_beng(title, content) WITH PARSER beng;")
    run("ALTER TABLE docs ADD FULLTEXT INDEX fti_content(content) WITH PARSER beng;")

    results = {}
    def snapshot(prefix, expected):
        query = "MATCH(title, content) AGAINST('quartz')"
        results[prefix + "_scores"] = run(
            f"SELECT id,CAST({query} AS DECIMAL(30,12)) FROM docs WHERE {query} ORDER BY id;")
        ids = [int(row.split("\t")[0]) for row in results[prefix + "_scores"]]
        check(ids == expected, f"Incorrect independently expected IDs at {prefix}")
        results[prefix + "_top20"] = run(
            f"SELECT id,CAST({query} AS DECIMAL(30,12)) AS score FROM docs "
            f"WHERE {query} ORDER BY score DESC,id ASC LIMIT 20;")
        results[prefix + "_limit_count"] = run(
            "SELECT COUNT(*) FROM (SELECT id FROM docs WHERE "
            "MATCH(content) AGAINST('quartz') LIMIT 20) q;")
        check(results[prefix + "_limit_count"] == [str(min(20, len(expected)))],
              "Incorrect outer COUNT/LIMIT result")
        check(run("SELECT COUNT(*) FROM docs WHERE MATCH(title, content) "
                  "AGAINST('nevermatchingword');") == ["0"], "Unexpected no-hit result")
    expected = [i for i in range(1, count + 1) if i % 4 in (0, 1)]
    snapshot("built", expected)
    run(f"INSERT INTO docs VALUES ({count + 1},'new report','quartz zephyrmarker');"
        "UPDATE docs SET content='quartz replacement' WHERE id=2; DELETE FROM docs WHERE id=4;")
    expected = sorted((set(expected) | {2, count + 1}) - {4})
    snapshot("dml", expected)
    check(run("SELECT id FROM docs WHERE MATCH(title, content) AGAINST('zephyrmarker');") ==
          [str(count + 1)], "Newly inserted token not searchable")

    run("CREATE TABLE empty_docs(id INT PRIMARY KEY, body TEXT);"
        "ALTER TABLE empty_docs ADD FULLTEXT INDEX fti_empty(body) WITH PARSER beng;")
    check(run("SELECT COUNT(*) FROM empty_docs WHERE MATCH(body) AGAINST('quartz');") == ["0"],
          "Empty-table FTS result is not empty")
    run("INSERT INTO empty_docs VALUES (1,'quartz'),(2,''),(3,NULL),(4,'QUARTZ');")
    check(run("SELECT id FROM empty_docs WHERE MATCH(body) AGAINST('quartz') ORDER BY id;") ==
          ["1", "4"], "Short/empty/NULL text results differ")

    for table in ("concurrent_a", "concurrent_b"):
        run(f"CREATE TABLE {table}(id INT PRIMARY KEY, title VARCHAR(128), content TEXT);"
            f"INSERT INTO {table} SELECT * FROM docs WHERE id<=512;")
    with ThreadPoolExecutor(max_workers=2) as pool:
        futures = [pool.submit(run, f"ALTER TABLE {table} ADD FULLTEXT INDEX fti_{table}"
                               "(title,content) WITH PARSER beng;")
                   for table in ("concurrent_a", "concurrent_b")]
        for future in futures:
            future.result()
    for table in ("concurrent_a", "concurrent_b"):
        actual = run(f"SELECT id FROM {table} WHERE MATCH(title,content) "
                     "AGAINST('quartz') ORDER BY id;")
        check(actual == [str(i) for i in expected if i <= 512],
              f"Concurrent DDL results differ for {table}")
        results[table] = actual
    return results


def run_mode(binary, mysql, output, mode, count):
    directory = output / mode
    runtime = directory / "runtime"
    runtime.mkdir(parents=True)
    env = os.environ.copy()
    for name in GATES:
        env.pop(name, None)
    if mode != "off":
        env[GATES[0]] = "1"
    if mode == "queue_off":
        env[GATES[1]] = "0"
    command = [str(binary), "--nodaemon", "--base-dir", str(runtime), "--log-level",
               "ALL.*:ERROR,RS.*:INFO,SHARE.*:INFO",
               "--parameter", "memory_budget=2G", "--parameter", "cpu_count=4",
               "--parameter", "syslog_io_bandwidth_limit=128M",
               "--parameter", "mysql_port_mode=disabled", "--parameter", "datafile_size=256M",
               "--parameter", "datafile_maxsize=2G", "--parameter", "log_disk_size=2G"]
    save(directory / "invocation.json", {"command": command,
         "environment": {name: env.get(name) for name in GATES}, "notice": NOTICE})
    with (directory / "server-output.log").open("w") as log:
        server = subprocess.Popen(command, cwd=runtime, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 90
            while True:
                check(server.poll() is None, f"{mode} server exited before readiness: {server.returncode}")
                try:
                    check(sql(mysql, runtime, "SELECT 1;", timeout=4) == ["1"], "Readiness mismatch")
                    break
                except (RuntimeError, subprocess.TimeoutExpired):
                    check(time.monotonic() < deadline, f"{mode} server readiness timed out")
                    time.sleep(0.25)
            result = workload(mysql, runtime, count)
            check(server.poll() is None, f"{mode} server exited during the workload")
            save(directory / "query-results.json", result)
        finally:
            if server.poll() is None:
                # seekdb deliberately maps SIGTERM to SIGKILL; SIGUSR1 drains
                # and stops its runtime (ObSignalHandle::deal_signals).
                server.send_signal(signal.SIGUSR1)
                try:
                    server.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()
                    raise RuntimeError(f"{mode} server failed to shut down within 30 seconds")
            save(directory / "shutdown.json", {"returncode": server.returncode})
        check(server.returncode == 0, f"{mode} server exited with {server.returncode}")
    evidence = collect_ddl(runtime, directory)
    workers = {job["worker"] for job in evidence["jobs"]}
    if mode == "on":
        check("IkDdlPair" in workers, "BEng opt-in never executed a companion task")
        check(any(pair["overlap_observed"] for pair in evidence["pairs"].values()),
              "No main BEng pair actually overlapped; increase fixture size and inspect logs")
    else:
        check(workers == {"DdlBuild"}, f"Unexpected companion execution in {mode}")
        check(not any(pair["overlap_observed"] for pair in evidence["pairs"].values()),
              f"Unexpected pair overlap in {mode}")
    print(f"PASS {mode}: SQL, DML, empty/short/NULL, concurrent DDL, shutdown, worker evidence", flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--mysql", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rows", type=int, default=4096)
    args = parser.parse_args()
    check(args.rows >= 512 and args.rows % 4 == 0, "--rows must be >=512 and divisible by 4")
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    binary, mysql = args.binary.resolve(strict=True), args.mysql.resolve(strict=True)
    save(args.output / "identity.json", {"binary": str(binary), "rows": args.rows,
         "sha256": hashlib.sha256(binary.read_bytes()).hexdigest(), "notice": NOTICE})
    modes = {mode: run_mode(binary, mysql, args.output, mode, args.rows)
             for mode in ("off", "on", "queue_off")}
    check(modes["off"] == modes["on"] == modes["queue_off"],
          "Cross-mode query results / BM25 scores / ordering differ")
    save(args.output / "summary.json", {"passed": True, "modes": list(modes),
         "cross_mode_exact_results": True, "notice": NOTICE})
    print("PASS all three modes: exact result sets, rounded BM25 scores and deterministic ordering", flush=True)
    print(NOTICE, flush=True)


if __name__ == "__main__":
    main()
