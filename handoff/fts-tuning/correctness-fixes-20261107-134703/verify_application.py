"""Verify targeted results using the exact approved files in the working tree."""
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile

HERE=Path(__file__).resolve().parent
REPO=HERE.parents[2]


def digest(b):
    return hashlib.sha256(b).hexdigest()


def verify():
    info=json.loads((HERE/"application-archive-manifest.json").read_text())
    arc=HERE/"application-evidence.tar.gz"
    assert digest(arc.read_bytes())==info["sha256"]
    with tarfile.open(arc,"r:gz") as t:
        members=t.getmembers()
        assert len(members)==len({m.name for m in members})==info["files"]
        assert all(m.isfile() and not Path(m.name).is_absolute()
                   and ".." not in Path(m.name).parts for m in members)
        raw={m.name:t.extractfile(m).read() for m in members}
    cfg=json.loads(raw["application-manifest.json"])
    patch=json.loads((HERE/"patch-manifest.json").read_text())
    assert set(cfg)==set(patch)
    for path,rec in cfg.items():
        assert rec["sha256"]==patch[path]["candidate_sha256"]
        assert digest(raw["applied-fixtures/"+rec["file"]])==rec["sha256"]
        assert digest((REPO/path).read_bytes())==rec["sha256"]
    cases=json.loads(raw["applied-targeted/case-summary.json"])
    assert [r["case"] for r in cases]==["basic_dml","partitioned_simple_query"]
    assert [r["rc"] for r in cases]==[0,0]
    base="tools/deploy/mysql_test/test_suite/fts_index"
    for row in cases:
        case=row["case"]
        assert row["test_sha256"]==digest((REPO/base/"t"/(case+".test")).read_bytes())
        assert row["expected_sha256"]==digest((REPO/base/"r/mysql"/(case+".result")).read_bytes())
    assert raw["applied-targeted.rc"].strip()==b"0"
    node=json.loads(raw["node-application-final.json"])
    assert node["stop"]["normal"] and node["stop"]["returncode"]==0 and node["stop"]["signal"]=="SIGUSR1"
    assert node["ports"]["out"]=="" and node["owned_tmux"]==[] and node["node_original_fixtures_preserved"]
    subprocess.run(["git","diff","--quiet","194150497aca9a69e395317bd6d5a64f26340a61",
                    "--","src","rust","deps","tools/benchmark"],cwd=REPO,check=True)
    return dict(archive_sha256=info["sha256"],archive_files=len(raw),targeted_passed=2,
                targeted_failed=0,cases=cases,resources=node["resources"],
                node_original_fixtures_preserved=True,product_and_benchmark_unchanged=True,
                node_data=node["data_allocated"],normal_stop=True)


if __name__=="__main__":
    print(json.dumps(verify(),indent=2,ensure_ascii=False))
