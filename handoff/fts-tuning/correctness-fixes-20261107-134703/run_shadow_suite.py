"""Validate all three suites with exactly the proposed FTS fixture changes."""
import json
import signal

import legacy_suite_helper as suites
from run_cases import HERE, Retest, backend, interrupted, run_case


def main():
    assert (HERE / "shadow-red.rc").read_text().strip() == "0"
    session = Retest()
    session.preflight()
    original = json.loads((HERE / "original-fixtures.json").read_text())
    patch = json.loads((HERE / "patch-manifest.json").read_text())
    for path, digest in original.items():
        assert backend.sha(session.src/path) == digest, path
    for path, record in patch.items():
        assert backend.sha(session.src/path) == record["original_sha256"], path
        assert backend.sha(HERE/record["shadow"]) == record["candidate_sha256"], path
    official = session.src / "tools/deploy/mysql_test/test_suite/fts_index"
    out = HERE / "shadow-green"
    with session.instance("shadow-green", out) as (_, data):
        setup = ("CREATE DATABASE IF NOT EXISTS test; "
                 "CREATE USER IF NOT EXISTS 'admin' IDENTIFIED BY ''; "
                 "GRANT ALL PRIVILEGES ON *.* TO 'admin';")
        backend.checked(backend.command(session.sql+["-e",setup],timeout=20))
        fts_out = out / "fts_index"
        fts_out.mkdir()
        fts=[]
        for source in sorted((official/"t").glob("*.test")):
            case = source.stem
            expected = official / "r/mysql" / (case+".result")
            test_record = patch.get(str(source.relative_to(session.src)))
            result_record = patch.get(str(expected.relative_to(session.src)))
            test = HERE/test_record["shadow"] if test_record else source
            answer = HERE/result_record["shadow"] if result_record else expected
            fts.append(run_case(session,data,fts_out,case,test,answer))
        suites.R,suites.S,suites.O,suites.PORT = session.run,session.src,out,session.port
        suites.CLIENT=session.client.parent
        suites.M,suites.MT=str(session.client),str(session.client.with_name("mysqltest"))
        suites.E["PATH"]=str(suites.CLIENT)+":"+suites.E["PATH"]
        suites.sql=lambda text,force=False: backend.command(session.sql+(["--force"] if force else [])+["-e",text],timeout=180)
        records=dict(fts_index=fts,file_sql=suites.suite("shadow-green",data,"file_sql"),
                     ai_column=suites.suite("shadow-green",data,"ai_column"))
        stats={}
        for suite,rows in records.items():
            failed=[r["case"] for r in rows if r["rc"]]
            stats[suite]=dict(total=len(rows),passed=len(rows)-len(failed),failed=failed)
        for path,digest in original.items():
            assert backend.sha(session.src/path)==digest,path
        backend.write_json(out/"summary.json",dict(stats=stats, records=records,
            product_reference=session.cfg["source_product_sha"], shadow_only=True,
            official_original_bytes_unchanged=True, full_platform_acceptance=False))
        print("SHADOW_SUITE",json.dumps(stats,ensure_ascii=False),flush=True)


if __name__ == "__main__":
    signal.signal(signal.SIGTERM,interrupted)
    main()
