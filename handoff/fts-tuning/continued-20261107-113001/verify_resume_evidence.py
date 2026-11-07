"""Cross-check direct observations and calibration records without running SeekDB."""
import hashlib
import json
import math
from pathlib import Path
import tarfile

ROOT = Path(__file__).resolve().parent
PREVIOUS = ROOT.parent / "next-cycle-20261107-104032"
LINUX = "9544ca38c4d6cc336adb079f362eff1fa220739d58ce70bc2d0f1d948d5ade32"
NATIVE = "6e3f898d7f45744e1dacfd7cd4a91da3a91d593ff4cad7823b76265cb1f5f600"


def obj(f, n):
    return json.load(f.extractfile(n))


def get(f, n):
    return f.extractfile(n).read()


def identity(f, path):
    inst = obj(f, path + "/instance.json")
    stop = obj(f, path + "/stopped.json")
    assert inst["binary_sha256"] == LINUX
    assert stop["gdb_rc"] == 0 and stop["stop_signal"] == "SIGUSR1" and not stop["listeners"]
    assert not stop["resource_errors"]
    return inst


def check_record(x, group_count=1):
    assert x["rc"] == 0 and not x["probe_errors"] and len(x["groups"]) == group_count
    for g in x["groups"]:
        assert g["score_allocated"] and g["doc_allocated"]
        assert g["score_capacity"] == g["score_count"] == g["doc_capacity"] == g["doc_count"] == g["batch"]
        assert g["score_payload_bytes"] == g["batch"] * g["token_inits"] * 8
        assert g["docid_payload_bytes"] == g["batch"] * g["token_inits"] * 52


def previous_match(current, prior):
    assert all(current[k] == prior[k] for k in ["rc", "stdout_sha256", "stderr_sha256"])


def main():
    prior = json.loads((PREVIOUS / "prior-reference.json").read_text())["edges"]
    checks = {}
    with tarfile.open(ROOT / "allocation-attempt1.tar.gz") as f:
        i = identity(f, ".")
        assert obj(f, "./stopped.json")["results"] == 9
        assert b"CREATE DATABASE edges;USE edges;" in get(f, "./probe-driver.log")
        names = ["empty", "single", "tokens_255", "tokens_256", "tokens_257", "tokens_1024",
                 "prepared_crossing", "lookup_projection", "forced_primary_lookup"]
        for n in names:
            x = obj(f, "./" + n + ".json")
            check_record(x, 0 if n == "empty" else 1)
            if n in prior:
                previous_match(x, prior[n])
            if n in ("lookup_projection", "forced_primary_lookup"):
                assert not x["groups"][0]["scoreless"]
            if n.startswith("tokens_"):
                g = x["groups"][0]
                assert g["scoreless"] and g["batch"] == 16
                assert g["token_inits"] == int(n.removeprefix("tokens_"))
            checks[n] = x
    with tarfile.open(ROOT / "allocation-followup.tar.gz") as f:
        assert identity(f, "followup")["fixture_sha256"] == i["fixture_sha256"]
        cases = obj(f, "followup/allocation.json")
        for n, x in cases.items():
            check_record(x)
            assert not x["doc_load_errors"] and x["doc_load_ids_in_case"]
            assert x["groups"][0]["scoreless"] and x["groups"][0]["batch"] == 16
        for n in ("literal_1024", "prepared_1024", "prepared_1024_no_hits"):
            x = cases[n]
            assert x["groups"][0]["token_inits"] == 1024
            assert x["observed_doc_buffers_written"] == (0 if n.endswith("no_hits") else 1)
            expected = b"0\n" if n.endswith("no_hits") else b"1000\n"
            assert get(f, "followup/" + n + ".out") == expected
        previous_match(cases["tiny_partition"], prior["tiny_partition"])
    with tarfile.open(ROOT / "allocation-short-first.tar.gz") as f:
        assert identity(f, "short-first")["fixture_sha256"] == i["fixture_sha256"]
        x = obj(f, "short-first/allocation.json")["prepared_short_then_1024"]
        check_record(x)
        assert not x["doc_load_errors"] and x["doc_load_ids_in_case"]
        assert get(f, "short-first/prepared_short_then_1024.out") == b"1000\n" * 3
        raw = get(f, "short-first/gdb.log").decode()
        rec = [json.loads(line.removeprefix("FTS_ARRAY ")) for line in raw.splitlines()
               if line.startswith("FTS_ARRAY ")]
        used = {json.loads(line.removeprefix("FTS_DOC_LOAD "))["init_id"] for line in raw.splitlines()
                if line.startswith("FTS_DOC_LOAD ")}
        assert len(rec) == x["groups"][0]["token_inits"] == 1026
        assert all(y["batch"] == 256 and y["scoreless"] for y in rec)
        assert len(used) == x["observed_doc_buffers_written"] == 3
        assert rec[0]["init_id"] in used and rec[-1]["init_id"] in used
        mid = rec[1:-1]
        n_loaded = sum(y["init_id"] in used for y in mid)
        assert n_loaded == 1
        short_first = dict(batch=256, terms=len(mid), score_payload=len(mid) * 256 * 8,
                           docid_payload=len(mid) * 256 * 52, filled=n_loaded,
                           unfilled_docid_payload=(len(mid)-n_loaded) * 256 * 52,
                           three_statement_groups=x["groups"])

    local = json.loads((ROOT / "native-counter-aa.json").read_text())
    ident = json.loads((ROOT / "native-identity.json").read_text())
    stop = json.loads((ROOT / "native-stopped.json").read_text())
    assert ident["binary_sha256"] == NATIVE and ident["fixture_sha256"] == i["fixture_sha256"]
    assert stop["rc"] == 0 and stop["signal"] == "SIGUSR1" and stop["blocks"] == 4
    for k, v in local["pairs"].items():
        blocks = [json.loads((ROOT / f"native-block-{i}.json").read_text())[k] for i in range(1, 5)]
        assert len({r["stdout_sha256"] for r in blocks}) == 1
        assert all((r["after"]["instructions"] - r["before"]["instructions"]) / r["n"] ==
                   v["instructions_per_stmt"][i] for i, r in enumerate(blocks))
    assert max(abs(v) for w in local["pairs"].values() for v in w["counter_pair_delta_pct"]) > .6

    with tarfile.open(ROOT / "transport-pilot.tar.gz") as f:
        pilot = obj(f, "transport-pilot/result.json")
        ident = obj(f, "transport-pilot/identity.json")
        stop = obj(f, "transport-pilot/stopped.json")
        assert ident["binary_sha256"] == LINUX and ident["rounds"] == 3000
        assert stop["rc"] == 0 and stop["signal"] == "SIGUSR1" and not stop["listeners"]
        assert not pilot["qualifies_to_try_official_uds_aa"]
        for which, suite in pilot["pairs"].items():
            for k, v in suite.items():
                a = [obj(f, f"transport-pilot/block-{i}.json")[which][k] for i in range(1, 5)]
                assert a == v["ms"]
                assert all(math.isclose((a[j+1]/a[j]-1)*100, v["pair_delta_pct"][i])
                           for i, j in enumerate((0,2)))
                v["same_order_change_pct"] = [(a[j+2]/a[j]-1)*100 for j in (0,1)]

    node = json.loads((ROOT / "node-final-state.json").read_text())
    assert node["reference"] == LINUX and not node["active_owned"] and not node["test_listeners"]["out"]
    expected = json.loads((PREVIOUS / "reference-manifest.json").read_text())["files"]
    assert all(expected[f] == s for f, s in node["sources"].items())
    assert node["fixture"] == i["fixture_sha256"]
    if (ROOT / "fixture.json.gz").is_file():
        import gzip
        assert hashlib.sha256(gzip.decompress((ROOT / "fixture.json.gz").read_bytes())).hexdigest() == node["fixture"]
    print(json.dumps(dict(first=checks, followup=cases, short_first=short_first,
                          native=local, transport=pilot, new_experiments=0, formal_ab_pairs=0),
                     indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
