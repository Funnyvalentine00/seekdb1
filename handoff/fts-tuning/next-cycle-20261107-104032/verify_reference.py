"""Recheck the sealed reference using the established SQL fixtures and suites."""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import signal
import sys

import legacy_suite_helper as v
from reference_probe import Session, command, sha, terminate, write_json


def main():
    signal.signal(signal.SIGTERM, terminate)
    work = Path(__file__).resolve().parent
    session = Session(work / "reference-manifest.json")
    session.preflight()
    label = "final-reference"
    out = work / label
    v.R, v.S, v.O, v.PORT = session.run, session.src, out, session.port
    v.CLIENT = session.client.parent
    v.M, v.MT = str(session.client), str(session.client.with_name("mysqltest"))
    v.E["PATH"] = str(v.CLIENT) + ":" + v.E["PATH"]

    def sql(q, force=False):
        return command(session.sql + (["--force"] if force else []) + ["-e", q], timeout=180)

    def digest(b):
        return hashlib.sha256(b).hexdigest()

    def record(result):
        return dict(rc=result.returncode, stdout_sha256=digest(result.stdout),
                    stderr_sha256=digest(result.stderr))

    def probe(name, q):
        result = sql(q)
        for ext, b in [("out", result.stdout), ("err", result.stderr)]:
            with (out / f"{name}.{ext}").open("xb") as f:
                f.write(b)
        return record(result)

    v.sql = sql
    fixtures = [session.run / name for name in ["projection.sql", "score-controls.sql", "tokenize.sql", "edge-cases.json"]]
    agg = work / "v5-word-aggregation.sql"
    prior = json.loads((work / "prior-reference.json").read_text())
    with session.instance(label, out) as (p, data):
        write_json(out / "fixture-identity.json", {str(x): sha(x) for x in fixtures + [agg]})
        write_json(out / "official-summary.json", session.benchmark(label, out, p))
        summary = dict(semantics=v.capture_semantics(label), edges=v.edge_probes(label, p.pid))
        tokens = (session.run / "tokenize.sql").read_text().splitlines()
        summary["tokenize"] = [dict(line=i, **probe(f"tokenize-{i}", tokens[0] + "\n" + q))
                               for i, q in enumerate(tokens[1:], 2)]
        lines = agg.read_text().splitlines()
        split = next(i for i, line in enumerate(lines) if line.startswith("DROP DATABASE"))
        summary["aggregation"] = [dict(line=i, **probe(f"aggregate-{i}", lines[0] + "\n" + q))
                                  for i, q in enumerate(lines[1:split], 2)]
        summary["dml"] = probe("dml", lines[0] + "\n" + "\n".join(lines[split:]))
        queries = [
            "SET NAMES utf8mb4; SELECT tokenize('resume résumé resumé RESUME résumé','beng');",
            "SET NAMES utf8mb4; SELECT tokenize('数据库 database ＡＢＣ abc 数据库','ik');",
        ]
        reference = {q: record(sql(q)) for q in queries}

        def concurrent_result(i):
            q = queries[i % len(queries)]
            return dict(index=i, statement=q, result=record(sql(q)))

        with ThreadPoolExecutor(max_workers=4) as pool:
            responses = list(pool.map(concurrent_result, range(80)))
        write_json(out / "concurrency-responses.json", responses)
        summary["concurrency"] = dict(requests=80, workers=4, reference=reference,
                                      all_match=all(r["result"] == reference[r["statement"]] for r in responses))
        summary["regression"] = {name: v.suite(label, data, name)
                                 for name in ["fts_index", "file_sql", "ai_column"]}
        write_json(out / "reference-check.json", summary)
        sections = ["semantics", "tokenize", "aggregation", "dml", "concurrency"]
        differences = [s for s in sections if summary[s] != prior[s]]
        for name in sorted(set(summary["edges"]) | set(prior["edges"])):
            for field in ["rc", "stdout_sha256", "stderr_sha256"]:
                if summary["edges"].get(name, {}).get(field) != prior["edges"].get(name, {}).get(field):
                    differences.append(f"edge:{name}:{field}")
        stats = {}
        for suite, records in summary["regression"].items():
            status = {x["case"]: x["rc"] for x in records}
            prev_status = {x["case"]: x["rc"] for x in prior["regression"][suite]}
            if status != prev_status:
                differences.append("regression:" + suite)
            failed = [name for name, rc in status.items() if rc]
            stats[suite] = dict(total=len(status), passed=len(status) - len(failed), failed=failed)
        write_json(out / "comparison.json", dict(previous_evidence="v5-20261107/final-evidence.tar.gz",
                   differences=differences, stats=stats))
        print("REFERENCE_RECHECK", json.dumps(dict(stats=stats, differences=differences)), flush=True)


if __name__ == "__main__":
    main()
