"""Verify supplied cloud evidence offline; never execute or extract its probe."""
import base64
import collections
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
ARCHIVE = HERE / "seekdb-gate-bkv6cbe6.zip"
EXPECTED_ARCHIVE = "f0a8c1c15f9493b679d5657d02b0210883ebe7bc7f892b0ccc56a08ea4f73a97"
V3 = "7accd1ace8ee5d035b3ef8aaf426c46b1d0a595e"
PRODUCT = "194150497aca9a69e395317bd6d5a64f26340a61"


def verify():
    archive_hash = hashlib.sha256(ARCHIVE.read_bytes()).hexdigest()
    assert archive_hash == EXPECTED_ARCHIVE
    with zipfile.ZipFile(ARCHIVE) as z:
        assert z.testzip() is None
        assert len(z.namelist()) == len(set(z.namelist())) == 11
        checked = []
        for line in z.read("SHA256SUMS").decode().splitlines():
            wanted, name = line.split(None, 1)
            name = name.lstrip("* ")
            assert hashlib.sha256(z.read(name)).hexdigest() == wanted, name
            checked.append(name)
        assert len(checked) == len(set(checked)) == 10
        assert set(checked) == set(z.namelist()) - {"SHA256SUMS"}
        sources = json.loads(z.read("source_response.json"))
        manifest = json.loads(z.read("manifest.json"))
        assert sources["commit"] == manifest["commit"] == V3
        hashes = {}
        assert len(sources["files"]) == len(manifest["files"]) == 2
        for item, recorded in zip(sources["files"], manifest["files"]):
            assert item["encoding"] == "base64"
            raw = base64.b64decode("".join(item["content"].split()), validate=True)
            name = item["path"]
            assert recorded["path"] == name
            assert raw == z.read(name)
            digest = hashlib.sha256(raw).hexdigest()
            blob = hashlib.sha1(b"blob " + str(len(raw)).encode() + b"\0" + raw).hexdigest()
            assert digest == recorded["sha256"]
            assert blob == item["sha"] == recorded["sha"]
            assert recorded["byte_count"] == len(raw)
            for ref in (V3, PRODUCT):
                committed = subprocess.check_output(["git", "show", ref + ":" + name], cwd=REPO)
                assert raw == committed, (ref, name)
            assert raw == (REPO / name).read_bytes(), name
            hashes[name] = digest
        raw_log = z.read("probe.raw.jsonl")
        assert raw_log == z.read("probe.stdout.jsonl")
        assert z.read("probe.stderr.txt") == b""
        rows = [json.loads(line) for line in raw_log.splitlines()]
        assert len(rows) == 76
        env = rows[0]
        assert env["stage"] == "environment"
        assert hashlib.sha256(z.read("socket_probe.py")).hexdigest() == env["probe_sha256"]
        summaries = []
        for place in ("workspace", "tmp"):
            group = [r for r in rows if r.get("location") == place]
            for stage in ("mkdtemp_exclusive", "chdir", "mkdir_run"):
                setup = [r for r in group if r.get("protocol") == "setup" and r["stage"] == stage]
                assert len(setup) == 1 and setup[0]["status"] == "passed"
            unix = [r for r in group if r.get("protocol") == "AF_UNIX/SOCK_STREAM"]
            failure = [r for r in unix if r["stage"] == "socket"]
            assert len(failure) == 1
            failure = failure[0]
            assert (failure["status"], failure["errno"], failure["errno_name"]) == ("failed", 1, "EPERM")
            assert failure["exception_type"] == "PermissionError"
            downstream = [r for r in unix if r["stage"] not in ("socket", "summary")]
            assert len(downstream) == 13
            assert all(r["status"] == "not_attempted" for r in downstream)
            assert {r["stage"] for r in downstream} >= {"bind", "listen", "connect", "accept"}
            end = [r for r in unix if r["stage"] == "summary"]
            assert len(end) == 1 and end[0]["failed_stage"] == "socket"
            tcp = [r for r in group if r.get("protocol") == "AF_INET/SOCK_STREAM"]
            assert len(tcp) == 18 and all(r["status"] == "passed" for r in tcp)
            for r in tcp:
                if r["stage"] in ("server_recv", "client_recv"):
                    assert r["result"]["received_hex"] == r["result"]["expected_hex"]
            summaries.append(dict(location=place, unix_stage="socket", errno=1,
                                  errno_name="EPERM", unix_downstream_not_attempted=13,
                                  tcp_bidirectional_roundtrip=True))
        final = rows[-1]
        assert final["stage"] == "final_summary" and final["no_retries"] is True
        assert all(final[k] is False for k in ("seekdb_started", "installed", "compiled"))
        assert len(final["results"]) == 2
        assert all(r["unix_ok"] is False and r["tcp_ok"] is True for r in final["results"])
        return dict(archive_sha256=archive_hash, crc_verified=True, file_hashes_verified=10,
                    source_and_git_blob_hashes_verified=hashes,
                    source_matches_refs=[V3, PRODUCT], raw_log_records=len(rows),
                    raw_status_counts=dict(collections.Counter(r.get("status", "metadata") for r in rows)),
                    cases=summaries, cloud_probe_start_utc=env["utc"],
                    cloud_probe_end_utc=final["utc"], seekdb_executed=False,
                    archive_probe_executed_locally=False, local_archive_extraction=False,
                    decision="current cloud environment did not pass unchanged reference startup prerequisite",
                    mechanism_of_restriction="unknown", new_product_experiments=0,
                    cumulative_product_experiments=1, total_experiment_budget=8)


if __name__ == "__main__":
    print(json.dumps(verify(), ensure_ascii=False, indent=2))
