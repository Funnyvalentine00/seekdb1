"""Recheck published reference without an extra official timed benchmark."""
import hashlib
import json
import signal
import traceback

import legacy_suite_helper as suites
from run_calibration import HERE, Retest, backend, interrupted


def main():
    assert (HERE / "resume.rc").read_text().strip() == "0"
    assert json.loads((HERE / "aa-result-resumed.json").read_text())["reports"] == 4
    prior = json.loads((HERE / "prior-reference.json").read_text())
    session = Retest()
    session.preflight()
    tag, out = "functional-reference", HERE / "functional-reference"
    suites.R, suites.S, suites.O, suites.PORT = session.run, session.src, out, session.port
    suites.CLIENT = session.client.parent
    suites.M, suites.MT = str(session.client), str(session.client.with_name("mysqltest"))
    suites.E["PATH"] = str(suites.CLIENT) + ":" + suites.E["PATH"]

    def sql(text, force=False):
        return backend.command(session.sql + (["--force"] if force else []) + ["-e", text], timeout=180)

    suites.sql = sql
    with session.instance(tag, out) as (process, data):
        identities = {name: backend.sha(session.run / name)
                      for name in ("projection.sql", "score-controls.sql", "tokenize.sql", "edge-cases.json")}
        for name in ("fts_index", "file_sql", "ai_column"):
            root = session.src / "tools/deploy/mysql_test/test_suite" / name
            for part in ("t", "r/mysql"):
                for item in sorted((root / part).glob("*")):
                    if item.is_file():
                        identities[str(item.relative_to(session.src))] = backend.sha(item)
        backend.write_json(out / "fixture-identity.json", identities)
        result = dict(semantics=suites.capture_semantics(tag), edges=suites.edge_probes(tag, process.pid))
        result["regression"] = {name: suites.suite(tag, data, name)
                                for name in ("fts_index", "file_sql", "ai_column")}
        backend.write_json(out / "reference-check.json", result)
        differences = [] if result["semantics"] == prior["semantics"] else ["semantics"]
        for name in sorted(set(result["edges"]) | set(prior["edges"])):
            for key in ("rc", "stdout_sha256", "stderr_sha256"):
                if result["edges"].get(name, {}).get(key) != prior["edges"].get(name, {}).get(key):
                    differences.append("edge:" + name + ":" + key)
        stats = {}
        for name, rows in result["regression"].items():
            current = {r["case"]: r["rc"] for r in rows}
            old = {r["case"]: r["rc"] for r in prior["regression"][name]}
            if current != old:
                differences.append("regression:" + name)
            failed = [case for case, rc in current.items() if rc]
            stats[name] = dict(total=len(current), passed=len(current)-len(failed), failed=failed)
        summary = dict(stats=stats, differences_vs_recorded_reference=differences,
                       benchmark_not_repeated=True, full_product_acceptance=False,
                       note="Known failures remain failures even when identical to reference")
        backend.write_json(out / "comparison.json", summary)
        print("CORRECTNESS_RESULT", json.dumps(summary, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, interrupted)
    try:
        main()
    except Exception as exc:
        backend.write_json(HERE / "correctness-failure.json", dict(error=repr(exc), traceback=traceback.format_exc()))
        raise
