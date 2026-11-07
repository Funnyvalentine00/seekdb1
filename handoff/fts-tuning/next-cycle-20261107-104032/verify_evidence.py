"""Verify the closed execution window from the archived, read-only evidence."""
from datetime import datetime
import gzip
import hashlib
import json
from pathlib import Path
import re
import statistics
import tarfile

ROOT = Path(__file__).resolve().parent
BINARY = "9544ca38c4d6cc336adb079f362eff1fa220739d58ce70bc2d0f1d948d5ade32"
HITS = {"query_cn_hits": 8001, "query_beng_hits": 11000, "query_mixed_hits": 7332,
        "query_limit_hits": 20}
ORDER = ["build_ik_all_sec", "build_ik_content_sec", "build_beng_en_sec",
         "tokenize_ik_avg_ms", "tokenize_beng_avg_ms", "query_cn_avg_ms",
         "query_beng_avg_ms", "query_mixed_avg_ms", "query_limit_avg_ms", "select1_avg_ms"]


def read(f, name):
    return f.extractfile(name).read()


def obj(f, name):
    return json.loads(read(f, name))


def pct(a, b):
    return 100 * (b / a - 1)


def check_report(f, tag):
    run = obj(f, tag + "/official-summary.json")
    assert obj(f, tag + "/instance.json")["binary_sha256"] == BINARY
    assert all(run["metrics"][k] == v for k, v in HITS.items())
    assert run["benchmark_parameters"] == dict(ROWS="20000", BATCH="500", ROUNDS="3000",
            QUERY_ROUNDS="200", SAMPLES="3", WARMUP="30", SKIP_LOAD="0", OUTPUT="")
    assert read(f, tag + "/official.txt").count(b"FTS Large Benchmark Report") == 1
    return run


def calibration():
    with tarfile.open(ROOT / "aa-evidence.tar.gz") as f:
        first = obj(f, "aa-stopped-before-aa1a.json")
        assert first["max_load_pct"] == 16 and not any(x.endswith("/official.txt") for x in f.getnames())
    with tarfile.open(ROOT / "aa-w2-evidence.tar.gz") as f:
        tags = ["w2-aa1a", "w2-aa1b", "w2-aa2b"]
        report_tags = sorted(n.split("/")[0] for n in f.getnames() if n.endswith("/official.txt"))
        assert report_tags == sorted(tags)
        a, b, c = runs = [check_report(f, tag) for tag in tags]
        assert c["host_after"]["max_load_pct"] == 17
        tail_runs = [obj(f, t + "/tails.json") for t in tags]
        for block in tail_runs:
            assert len(block) == 7
            for vals in block.values():
                assert len(vals["latencies_ms"]) == vals["n"] == 1000
                q = statistics.quantiles(vals["latencies_ms"], n=100, method="inclusive")
                assert q[94] == vals["p95_ms"] and q[98] == vals["p99_ms"]
        pairs = {k: pct(a["metrics"][k], b["metrics"][k]) for k in ORDER}
        rss = [x["rss_benchmark"]["VmHWM"] / 2**20 for x in runs]
        tail = {name: {k: pct(tail_runs[0][name][k], tail_runs[1][name][k])
                       for k in ["p95_ms", "p99_ms"]} for name in tail_runs[0]}
        return dict(decision="STOP_INSUFFICIENT_MEASUREMENT_STABILITY", reports=3, complete_pairs=1,
                    partial_pair_tag=tags[-1], initial_attempt_host_load=first["max_load_pct"],
                    host_load_pairs=[[x["host_before"]["max_load_pct"], x["host_after"]["max_load_pct"]]
                                     for x in runs],
                    pair_changes_pct=pairs, aa_guardrail_pct=1.0, select1_guardrail_pct=2.0,
                    rss_guardrail_pct=2.0, rss_mib=rss, first_pair_rss_pct=pct(*rss[:2]),
                    first_pair_tail_pct=tail, tail_samples=21000, tags=tags)


def cold_record(archive, tag, expected):
    with tarfile.open(ROOT / archive) as f:
        assert obj(f, tag + "/instance.json")["binary_sha256"] == expected
        cold = obj(f, tag + "/cold_ik_all.json")
        warm = obj(f, tag + "/warm_ik_content.json")
        assert cold["rc"] == warm["rc"] == 0 and not cold["errors"]
        log = read(f, tag + "/ddl-log-markers.txt").decode().splitlines()
        ends = [line for line in log if "build_ranges_concurrently_thread_pool completed" in line]
        assert [int(re.search(r"all_tries.size\(\)=(\d+)", x)[1]) for x in ends] == [6, 1, 1]
        last = datetime.fromisoformat(ends[-1][1:27] + "+08:00").timestamp()
        peak = max(cold["status_samples"], key=lambda x: x["VmHWM"])
        d = dict(tag=tag, binary=expected, cold_elapsed_seconds=cold["elapsed_seconds"],
                 warm_elapsed_seconds=warm["elapsed_seconds"], pre_ddl_hwm_mib=cold["before"]["VmHWM"]/2**20,
                 peak_rss_mib=peak["VmHWM"]/2**20,
                 peak_hwm_offset_s=(peak["t_ns"]-cold["begin_ns"])/1e9,
                 dictionary_done_offset_s=last-cold["begin_ns"]/1e9,
                 sql_memory_labels=list(sorted({line.split("\t")[0] for s in cold["allocator_labels"]
                                                for line in s["rows"].splitlines()})))
        assert d["peak_hwm_offset_s"] > d["dictionary_done_offset_s"]
        if tag == "cold-instrumented":
            builds = [l for l in log if "FTS_NEXT_DIAG dat_before_cache" in l and "dict_type=1" in l]
            assert len(builds) == 6
            trie = next(l for l in log if "FTS_NEXT_DIAG tries_collected(dict_type=1" in l)
            def field(line, key):
                return int(re.search(key + r"=(\d+)", line)[1])
            resets = [l for l in log if "FTS_NEXT_DIAG dat_after_reset" in l]
            assert len(resets) == 8 and all(field(x, "arena_total") == 0 for x in resets)
            d["main_trie_used_bytes"] = field(trie, "arena_used")
            d["main_trie_total_bytes"] = field(trie, "arena_total")
            d["main_dat_sum_total_bytes"] = sum(field(l, "arena_total") for l in builds)
            d["main_cache_copy_payload_bytes"] = sum(field(l, "buffer_size") for l in builds)
            d["expansions_observed"] = sum("FTS_NEXT_DIAG dat_expand" in l for l in log)
        return d


def compare_known_failures(f):
    # The mysqltest diagnostic includes a leading partial line of a byte-budgeted tail.
    # Check that it is a suffix of the same SQL display line, and compare all complete lines.
    prior_path = ROOT.parent / "v5-20261107/final-evidence.tar.gz"
    status = {}
    with tarfile.open(prior_path) as old:
        for case in ("basic_dml", "partitioned_simple_query"):
            name = "regress/final-reference/fts_index/" + case + ".log"

            def split(raw):
                lines = raw.decode().splitlines()
                if case == "basic_dml":
                    idx = next(i for i, l in enumerate(lines) if l.startswith(
                        "More results from queries before failure can be found in /"))
                    partial = lines.pop(idx + 1)
                    lines[idx] = "More results from queries before failure can be found in <LOG>"
                else:
                    idx = next(i for i, l in enumerate(lines) if l.endswith("mysqltest: Result content mismatch"))
                    assert lines[idx + 1] == ""
                    partial = lines.pop(idx + 2)
                    lines = [l for l in lines if not l.startswith(("--- /", "+++ /"))]
                return [re.sub(r"^\[\d{4}-\d{2}-\d{2} [^]]+\]\s*", "", l) for l in lines], partial

            before, old_partial = split(read(old, "results/" + name))
            after, new_partial = split(read(f, "final-reference/" + name))
            assert before == after
            assert min(len(old_partial), len(new_partial)) > 8
            assert old_partial.endswith(new_partial) or new_partial.endswith(old_partial)
            status[case] = dict(complete_lines_and_failure_equal=True, partial_tail_suffix_equal=True)
    return status


def main():
    aa = calibration()
    node = json.loads((ROOT / "node-final-state.json").read_text())
    assert node["reference"] == BINARY and not node["source_mismatches"]
    assert not node["active_owned_processes"] and node["ports"]["out"] == ""
    assert len(node["owned_data"]) == 7 and all(x["exists"] and x["owner_marker"] for x in node["owned_data"])
    req = json.loads((ROOT / "cold-diagnostic-source.json").read_text())
    assert hashlib.sha256(gzip.decompress((ROOT / "cold-diagnostic.patch.gz").read_bytes())).hexdigest() == req["patch_sha256"]
    with tarfile.open(ROOT / "instrumented-evidence.tar.gz") as f:
        build = obj(f, "cold-diag-build.json")
        assert build["rc"] == 0 and build["sources_restored"] == req["source_before"]
    diagnostic = build["diagnostic_sha256"]
    assert node["sealed_diagnostic"] == node["cache_binary"] == diagnostic
    cold = [cold_record("cold-evidence.tar.gz", "cold2", BINARY),
            cold_record("instrumented-evidence.tar.gz", "cold-instrumented", diagnostic)]
    with tarfile.open(ROOT / "warm-profile-v2-evidence.tar.gz") as f:
        assert read(f, "warm-profile-v2.status").strip() == b"0 0"
        raw_sha = hashlib.sha256(read(f, "warm-profile.data")).hexdigest()
        assert raw_sha == read(f, "warm-profile-raw.sha256").split()[0].decode()
        report = read(f, "warm-profile-inclusive-v2.txt").decode()
        sample_count = int(re.search(r"# Samples:\s+(\d+)", report)[1])
        rows = [l.strip() for l in report.splitlines() if re.match(r"^\s*\d+\.\d+%\s", l)]
        profile = dict(event="cpu-clock:u", samples=sample_count, raw_sha256=raw_sha,
                       output_lookup_symbol_found=any("get_output_fts_col_expr_by_type" in l for l in rows),
                       fill_generated_inclusive_rows=[l for l in rows if "ObTableScanOp::fill_generated_fts_cols" in l],
                       fetch_next_inclusive_rows=[l for l in rows if "ObTableScanOp::fetch_next_fts_index_rows" in l])
    with tarfile.open(ROOT / "final-reference-evidence.tar.gz") as f:
        rec = check_report(f, "final-reference")
        compare = obj(f, "final-reference/comparison.json")
        assert compare["differences"] == []
        assert [compare["stats"][n]["passed"] for n in ["fts_index", "file_sql", "ai_column"]] == [13, 1, 5]
        assert [compare["stats"][n]["total"] for n in ["fts_index", "file_sql", "ai_column"]] == [15, 1, 5]
        ref = obj(f, "final-reference/reference-check.json")
        assert len(ref["edges"]) == 9 and all(x["rc"] == 0 for x in ref["edges"].values())
        assert len(ref["tokenize"]) == 22 and len(ref["aggregation"]) == 12 and ref["dml"]["rc"] == 0
        assert ref["concurrency"]["all_match"] and len(obj(f, "final-reference/concurrency-responses.json")) == 80
        final = dict(regressions=compare["stats"], exact_semantic_differences=compare["differences"],
                     boundary_count=9, tokenize_count=22, aggregation_count=12, concurrent_requests=80,
                     known_failures=compare_known_failures(f), hits=HITS,
                     rss_mib=rec["rss_benchmark"]["VmHWM"]/2**20,
                     tokenize_errors=sum(x["rc"] != 0 for x in ref["tokenize"]),
                     aggregation_errors=sum(x["rc"] != 0 for x in ref["aggregation"]))
    print(json.dumps(dict(aa=aa, cold=cold, profile=profile, final_reference=final,
                          full_gate_best_validated=None, new_experiments=0, formal_pairs=0),
                     ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
