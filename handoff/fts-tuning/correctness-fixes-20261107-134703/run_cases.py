"""Run original or shadow FTS fixtures on an owned sealed-reference instance."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import traceback

from run_calibration import HERE, Retest, backend, interrupted

CASES = ("basic_dml", "partitioned_simple_query")


def run_case(session, data, out, case, test, expected):
    target = out / case
    target.mkdir()
    deploy = session.src / "tools/deploy"
    env = dict(os.environ, PATH=str(session.client.parent)+":"+os.environ["PATH"],
               OBMYSQL_MS0="127.0.0.1", OBMYSQL_PORT=str(session.port), OBMYSQL_PWD="",
               OBMYSQL_USR="root", TENANT="mysql", OBSERVER_DIR=str(data),
               IS_BUSINESS="0", PWD=str(deploy))
    argv = [str(session.client.with_name("mysqltest")), "--host=127.0.0.1",
            "--port="+str(session.port), "--user=root", "--database=test", "--silent",
            "--tmpdir="+str(target), "--logdir="+str(target), "--tail-lines=25",
            "--test-file="+str(test), "--result-file="+str(expected)]
    with (target / "mysqltest.log").open("xb") as log:
        result = subprocess.run(argv, cwd=deploy, env=env, stdin=subprocess.DEVNULL,
                                stdout=log, stderr=subprocess.STDOUT, timeout=420)
    record = dict(case=case, rc=result.returncode, test=str(test), test_sha256=backend.sha(test),
                  expected=str(expected), expected_sha256=backend.sha(expected), command=argv)
    backend.write_json(target / "case.json", record)
    print("CASE", case, result.returncode, flush=True)
    return record


def probes(session, out):
    for label in ("metadata-probe", "join-probe"):
        sql = (HERE / (label+".sql")).read_bytes()
        result = backend.command(session.sql + ["--raw"], sql, timeout=60)
        (out / (label+".out")).write_bytes(result.stdout)
        (out / (label+".err")).write_bytes(result.stderr)
        backend.write_json(out / (label+".json"), dict(rc=result.returncode, sha=hashlib.sha256(sql).hexdigest()))
        backend.checked(result)
    lines = (out / "join-probe.out").read_text().splitlines()
    marks = {line:lines.index(line) for line in ("---plan---","---actual---","---match-rows---","---dim-rows---")}
    plan = "\n".join(lines[marks["---plan---"]+1:marks["---actual---"]])
    actual = [line.split("\t") for line in lines[marks["---actual---"]+1:marks["---match-rows---"]]]
    matches = [line.split("\t") for line in lines[marks["---match-rows---"]+1:marks["---dim-rows---"]]]
    dim = [line.split("\t") for line in lines[marks["---dim-rows---"]+1:]]
    expected = [[digest,seq,dseq,log,score] for digest,seq,score in matches for dseq,log in dim
                if int(seq)<int(dseq)]
    expected.sort(key=lambda r:(int(r[1]),int(r[2])))
    backend.write_json(out / "join-oracle.json", dict(rows=len(actual), matches=len(matches),
                      dim_rows=len(dim), exact=actual==expected, actual=actual, expected=expected,
                      nested_loop="NESTED-LOOP JOIN" in plan,
                      binding_1="nl_params_([t_dim_table.seq(:1)])" in plan,
                      range_1="range_cond([t_ir_single_column.seq < :1])" in plan))
    print("JOIN_ORACLE",len(actual),actual==expected,flush=True)


def main():
    cli = argparse.ArgumentParser()
    cli.add_argument("mode", choices=("red","shadow-red","shadow-green"))
    args = cli.parse_args()
    session = Retest()
    session.preflight()
    official = session.src / "tools/deploy/mysql_test/test_suite/fts_index"
    config = json.loads((HERE / "original-fixtures.json").read_text())
    for name, expected in config.items():
        assert backend.sha(session.src / name) == expected, name
    out = HERE / args.mode
    with session.instance(args.mode,out) as (_, data):
        setup = ("CREATE DATABASE IF NOT EXISTS test; "
                 "CREATE USER IF NOT EXISTS 'admin' IDENTIFIED BY ''; "
                 "GRANT ALL ON test.* TO 'admin';")
        backend.checked(backend.command(session.sql+["-e",setup],timeout=20))
        if args.mode == "red":
            probes(session,out)
        records=[]
        for case in CASES:
            test = official / "t" / (case+".test")
            expected = official / "r/mysql" / (case+".result")
            if case == "basic_dml" and args.mode != "red":
                test = HERE / "shadow" / "basic_dml.test"
            if args.mode == "shadow-green":
                expected = HERE / "shadow" / (case+".result")
            records.append(run_case(session,data,out,case,test,expected))
        backend.write_json(out / "case-summary.json",records)


if __name__ == "__main__":
    signal.signal(signal.SIGTERM,interrupted)
    try:
        main()
    except Exception:
        print(traceback.format_exc(),flush=True)
        raise
