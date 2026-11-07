"""Verify preserved originals, shadow fixes, full test results, and resource state."""
import difflib
import gzip
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
BASE = "493dc5bd9b0bb2b7669aadc23035dc54b5a2f12e"
PRODUCT = "194150497aca9a69e395317bd6d5a64f26340a61"
SEAL = "9544ca38c4d6cc336adb079f362eff1fa220739d58ce70bc2d0f1d948d5ade32"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def verify():
    archive = HERE / "linux-evidence.tar.gz"
    archive_manifest = json.loads((HERE / "archive-manifest.json").read_text())
    assert digest(archive.read_bytes()) == archive_manifest["sha256"]
    with tarfile.open(archive, "r:gz") as t:
        members = t.getmembers()
        assert len(members) == len({m.name for m in members})
        assert all(m.isfile() and not Path(m.name).is_absolute()
                   and ".." not in Path(m.name).parts
                   and Path(m.name).parts[0] != "data" for m in members)
        raw = {m.name:t.extractfile(m).read() for m in members}
    assert set(raw) == {entry["path"] for entry in archive_manifest["files"]}
    for entry in archive_manifest["files"]:
        data = raw[entry["path"]]
        assert len(data) == entry["bytes"] and digest(data) == entry["sha256"]

    def obj(name):
        return json.loads(raw[name])

    finals = obj("node-final-state.json")
    assert finals["source_product_sha"] == PRODUCT and finals["reference_sha256"] == SEAL
    assert finals["ports"]["out"] == "" and finals["owned_tmux"] == [] and finals["active_owned"] == []
    originals = obj("original-fixtures.json")
    assert originals == finals["original_fixtures"]
    before = {}
    for path, sha in originals.items():
        before[path] = subprocess.check_output(["git","show",BASE+":"+path],cwd=REPO)
        assert digest(before[path]) == sha
    manifest = obj("patch-manifest.json")
    assert manifest == json.loads((HERE/"patch-manifest.json").read_text())
    after = {}
    rendered = []
    for path, rec in manifest.items():
        assert rec["original_sha256"] == originals[path]
        after[path] = raw[rec["shadow"]]
        assert digest(after[path]) == rec["candidate_sha256"]
        rendered.append("diff --git a/"+path+" b/"+path+"\n")
        rendered.extend(difflib.unified_diff(before[path].decode().splitlines(True),
            after[path].decode().splitlines(True),fromfile="a/"+path,tofile="b/"+path,n=3))
    assert "".join(rendered).encode() == gzip.decompress((HERE/"test-fixes.patch.gz").read_bytes())
    base = "tools/deploy/mysql_test/test_suite/fts_index"
    basic = base+"/r/mysql/basic_dml.result"
    partitioned = base+"/r/mysql/partitioned_simple_query.result"
    assert after[basic] == raw["shadow-red/basic_dml/basic_dml.reject"]
    assert after[partitioned] == raw["red/partitioned_simple_query/partitioned_simple_query.reject"]
    expected = before[partitioned]
    for old,new in ((b"nl_params_([t_dim_table.seq(:0)])",b"nl_params_([t_dim_table.seq(:1)])"),
                   (b"range_cond([t_ir_single_column.seq < :0]), has_functional_lookup=true",
                    b"range_cond([t_ir_single_column.seq < :1]), has_functional_lookup=true")):
        assert expected.count(old)==1
        expected=expected.replace(old,new)
    assert expected==after[partitioned]
    start=b"CREATE TABLE fts_parser_name_test (\n    title VARCHAR(200),\n    content TEXT,\n    FULLTEXT INDEX ctx(title, content)\n);\n"
    end=b"\nDROP TABLE fts_parser_name_test;\ncreate table fts_parser_name_test(seq_id"
    def region(data):
        assert data.count(start)==data.count(end)==1
        i=data.index(start)+len(start);j=data.index(end,i)
        return data[:i],data[i:j],data[j:]
    for path in (basic,base+"/t/basic_dml.test"):
        a,b=region(before[path]),region(after[path])
        assert a[0]==b[0] and a[2]==b[2] and a[1]!=b[1]
    assert [c["rc"] for c in obj("red/case-summary.json")]==[1,1]
    assert b"1064" in raw["red/basic_dml/mysqltest.log"]
    assert b"table_id = No such row" in raw["red/basic_dml/basic_dml.log"]
    assert b"Result length mismatch" in raw["shadow-red/basic_dml/mysqltest.log"]
    metadata=[line.split("\t") for line in raw["red/metadata-probe.out"].decode().splitlines()]
    ids=next(r for r in metadata if r[0]=="recreated_ids")
    assert ids[1]!=ids[2] and ids[3]=="1"
    assert ["stale_id_aux_count","0"] in metadata
    assert [r[1] for r in metadata if r[0]=="explicit_aux"]==["11","12","13","14"]
    assert [r[1:] for r in metadata if r[0]=="hidden_aux"]==[["13","space.1"],["14","space.1"]]
    assert ["hidden_base_columns","__pk_increment","1"] in metadata
    for record in (["after_insert","alpha,beta"],["after_update_old_term","beta"],
                   ["after_update_new_term","alpha"],["after_delete","0"]):
        assert record in metadata
    join=obj("red/join-oracle.json")
    assert join["exact"] and join["rows"]==24 and join["actual"]==join["expected"]
    assert join["nested_loop"] and join["binding_1"] and join["range_1"]
    result=obj("shadow-green/summary.json")
    assert result["shadow_only"] and result["official_original_bytes_unchanged"]
    assert result["product_reference"]==PRODUCT
    expected={"fts_index":15,"file_sql":1,"ai_column":5}
    for suite,n in expected.items():
        assert result["stats"][suite]==dict(total=n,passed=n,failed=[])
        assert len(result["records"][suite])==n
        assert all(record["rc"]==0 for record in result["records"][suite])
    records={r["case"]:r for r in result["records"]["fts_index"]}
    assert records["basic_dml"]["test_sha256"]==manifest[base+"/t/basic_dml.test"]["candidate_sha256"]
    assert records["basic_dml"]["expected_sha256"]==manifest[basic]["candidate_sha256"]
    assert records["partitioned_simple_query"]["expected_sha256"]==manifest[partitioned]["candidate_sha256"]
    assert records["partitioned_simple_query"]["test_sha256"]==originals[base+"/t/partitioned_simple_query.test"]
    for phase in ("red","shadow-red","shadow-green"):
        state=obj(phase+"/stopped.json")
        assert state["normal"] and state["returncode"]==0 and state["signal"]=="SIGUSR1"
    return dict(original_head=BASE,product=PRODUCT,binary_sha256=SEAL,
        archive_sha256=archive_manifest["sha256"],files_verified=len(raw),original_failures=2,
        hidden_docid_dml_verified=True,join_rows_exact=24,proposed_files=manifest,
        shadow_suite=result["stats"],shadow_tests_passed=21,shadow_tests_failed=0,
        original_files_preserved_during_trial=True,normal_stops=3,resources=finals["resources"],
        data_allocated=finals["data_allocated"],builds=0,new_performance_experiments=0,
        full_platform_acceptance=False)


if __name__ == "__main__":
    print(json.dumps(verify(),indent=2,ensure_ascii=False))
