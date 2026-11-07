"""Recompute all reported numbers from preserved Linux evidence, offline."""
import hashlib
import json
import math
from pathlib import Path
import statistics
import tarfile

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
TAGS = ("power-aa1a", "power-aa1b", "power-aa2b", "power-aa2a",
        "extra-aa3a", "extra-aa3b", "extra-aa4b", "extra-aa4a")
METRICS = ("select1_avg_ms", "build_ik_all_sec", "build_ik_content_sec", "build_beng_en_sec",
           "tokenize_ik_avg_ms", "tokenize_beng_avg_ms", "query_cn_avg_ms", "query_beng_avg_ms",
           "query_mixed_avg_ms", "query_limit_avg_ms")
HITS = dict(query_cn_hits=8001, query_beng_hits=11000, query_mixed_hits=7332, query_limit_hits=20)
T95 = {1:12.7062047364, 2:4.30265272975, 3:3.18244630528, 4:2.77644510520,
       5:2.57058183564, 6:2.44691185114, 7:2.36462425101}


def mean_stats(values):
    avg = statistics.mean(values)
    if len(values) < 2:
        return dict(n=len(values), mean=avg, sample_stdev=None, descriptive_iid_ci95=None)
    sd = statistics.stdev(values)
    half = T95[len(values)-1] * sd / math.sqrt(len(values))
    return dict(n=len(values), mean=avg, sample_stdev=sd,
                descriptive_iid_ci95=[avg-half, avg+half],
                relative_ci_halfwidth_pct=100*half/abs(avg) if avg else None)


def verify():
    archive = HERE / "linux-evidence.tar.gz"
    identity = json.loads((HERE / "archive-manifest.json").read_text())
    assert hashlib.sha256(archive.read_bytes()).hexdigest() == identity["sha256"]
    with tarfile.open(archive, "r:gz") as t:
        members = t.getmembers()
        names = [m.name for m in members]
        assert len(names) == len(set(names))
        assert all(m.isfile() and not Path(m.name).is_absolute()
                   and ".." not in Path(m.name).parts
                   and Path(m.name).parts[0] not in ("data", "data-extra") for m in members)
        raw = {m.name: t.extractfile(m).read() for m in members}
    assert set(raw) == {entry["path"] for entry in identity["files"]}
    for entry in identity["files"]:
        data = raw[entry["path"]]
        assert len(data) == entry["bytes"]
        assert hashlib.sha256(data).hexdigest() == entry["sha256"]
    def obj(name):
        return json.loads(raw[name])
    cfg = obj("reference-manifest.json")
    assert cfg["source_product_sha"] == "194150497aca9a69e395317bd6d5a64f26340a61"
    assert cfg["reference"]["sha256"] == "9544ca38c4d6cc336adb079f362eff1fa220739d58ce70bc2d0f1d948d5ade32"
    assert hashlib.sha256(raw["reference_backend.py"]).hexdigest() == cfg["backend_sha256"]
    for path, expected in cfg["files"].items():
        assert hashlib.sha256((REPO / path).read_bytes()).hexdigest() == expected, path
    present = [tag for tag in TAGS if tag + "/official-summary.json" in raw]
    assert present == list(TAGS[:len(present)])
    reports = [obj(tag + "/official-summary.json") for tag in present]
    tail_samples = 0
    for tag, report in zip(present, reports):
        assert report["benchmark_parameters"] == dict(ROWS="20000",BATCH="500",ROUNDS="3000",
            QUERY_ROUNDS="200",SAMPLES="3",WARMUP="30",SKIP_LOAD="0",OUTPUT="")
        assert all(report["metrics"][k] == v for k,v in HITS.items())
        instance = obj(tag + "/instance.json")
        assert instance["binary_sha256"] == cfg["reference"]["sha256"]
        assert instance["source_product_sha"] == cfg["source_product_sha"]
        stop = obj(tag + "/stopped.json")
        assert stop["normal"] is True and stop["returncode"] == 0 and stop["signal"] == "SIGUSR1"
        tails = obj(tag + "/tails.json")
        assert len(tails) == 7
        for entry in tails.values():
            samples = entry["latencies_ms"]
            assert entry["n"] == len(samples) == 1000
            assert all(v > 0 and math.isfinite(v) for v in samples)
            quantiles = statistics.quantiles(samples,n=100,method="inclusive")
            assert entry["p95_ms"] == quantiles[94] and entry["p99_ms"] == quantiles[98]
            tail_samples += len(samples)
    complete_pairs = len(reports)//2
    metrics, failed = {}, []
    for key in METRICS:
        values = [r["metrics"][key] for r in reports]
        assert all(v > 0 and math.isfinite(v) for v in values)
        changes = [100*(values[j+1]/values[j]-1) for j in range(0,2*complete_pairs,2)]
        stats = dict(values=values, **mean_stats(values), pair_delta_pct=changes)
        if changes:
            stats["pair_change_stats"] = mean_stats(changes)
        if len(changes) > 1:
            stats["fixed_idle_pairs_delta_pct"] = changes[1:]
            stats["fixed_idle_pair_change_stats"] = mean_stats(changes[1:])
        metrics[key] = stats
        if any(abs(v) > (2 if key == "select1_avg_ms" else 1) for v in changes):
            failed.append(key)
    rss = [r["rss_benchmark"]["VmHWM"]/1048576 for r in reports]
    rss_changes = [100*(rss[j+1]/rss[j]-1) for j in range(0,2*complete_pairs,2)]
    functional = obj("functional-reference/comparison.json")
    functional_stop = obj("functional-reference/stopped.json")
    assert functional_stop["normal"] is True and functional_stop["returncode"] == 0
    assert functional_stop["signal"] == "SIGUSR1"
    for name in ("run.rc", "resume.rc", "correctness.rc", "extra.rc"):
        assert raw[name].strip() == b"0"
    assert functional["differences_vs_recorded_reference"] == []
    assert functional["stats"] == dict(fts_index=dict(total=15,passed=13,failed=["basic_dml","partitioned_simple_query"]),
        file_sql=dict(total=1,passed=1,failed=[]),ai_column=dict(total=5,passed=5,failed=[]))
    edge_results = obj("functional-reference/reference-check.json")["edges"]
    assert len(edge_results) == 9 and all(v["rc"] == 0 for v in edge_results.values())
    for path, expected in obj("functional-reference/fixture-identity.json").items():
        if path.startswith("tools/"):
            assert hashlib.sha256((REPO/path).read_bytes()).hexdigest() == expected, path
    final = obj("node-final-state.json")
    assert final["reference_sha256"] == cfg["reference"]["sha256"]
    assert final["ports"]["out"] == "" and final["active_owned"] == []
    assert final["owned_tmux_sessions"] == []
    return dict(archive_sha256=identity["sha256"], archive_files=len(raw), source=cfg["source_product_sha"],
        reports=len(reports), complete_pairs=complete_pairs, hits_verified_each_run=HITS,
        tail_samples=tail_samples, metrics=metrics, failing_metric_gates=failed,
        rss_mib=rss, rss_pair_delta_pct=rss_changes, functional=functional,
        boundary_groups_passed=9, normal_instance_stops=len(present)+1, resource_final=final["resource"],
        notes=["first pair has mixed idle protocol; retained without exclusion from full history",
               "fixed_idle_pairs describes predeclared pairs 2 onward, not selected by outcome",
               "two owned datasets separate initial/functional and extra windows",
               "t intervals assume IID and stationarity; descriptive only if drift or order bias exists",
               "sample unit is full run or complete pair, never individual SQL latency"],
        new_product_experiments=0, cumulative_product_experiments=1, formal_candidate_pairs=0,
        best_validated_global=None, full_product_acceptance=False)


if __name__ == "__main__":
    print(json.dumps(verify(),ensure_ascii=False,indent=2))
