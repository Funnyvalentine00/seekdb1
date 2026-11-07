"""Retest the two cases using files copied from the approved live worktree."""
import json
import signal

from run_cases import HERE, Retest, backend, interrupted, run_case


def main():
    assert (HERE / "shadow-green.rc").read_text().strip()=="0"
    session=Retest()
    session.preflight()
    cfg=json.loads((HERE/"application-manifest.json").read_text())
    patch=json.loads((HERE/"patch-manifest.json").read_text())
    for path,rec in cfg.items():
        assert rec["sha256"]==patch[path]["candidate_sha256"],path
        assert backend.sha(HERE/"applied-fixtures"/rec["file"])==rec["sha256"],path
    out=HERE/"applied-targeted"
    original=session.src/"tools/deploy/mysql_test/test_suite/fts_index"
    f=HERE/"applied-fixtures"
    with session.instance("applied-targeted",out) as (_,data):
        backend.checked(backend.command(session.sql+["-e","CREATE DATABASE IF NOT EXISTS test;"],timeout=20))
        rows=[
            run_case(session,data,out,"basic_dml",f/"basic_dml.test",f/"basic_dml.result"),
            run_case(session,data,out,"partitioned_simple_query",original/"t/partitioned_simple_query.test",
                     f/"partitioned_simple_query.result"),
        ]
        backend.write_json(out/"case-summary.json",rows)
        if any(r["rc"]!=0 for r in rows):
            raise RuntimeError("applied-file retest failed; inspect individual logs")
        print("APPLIED_TARGETED 2/2",flush=True)


if __name__=="__main__":
    signal.signal(signal.SIGTERM,interrupted)
    main()
