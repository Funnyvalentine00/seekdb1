"""Verify coordination facts and historical archives without touching native jobs."""
from datetime import datetime, timezone, timedelta
from pathlib import Path
import hashlib,json,re,subprocess,zipfile

packet=Path(__file__).resolve().parent
project=packet.parents[2]
def git(*args):
    return subprocess.check_output(['git','--no-optional-locks',*args],cwd=project,stderr=subprocess.PIPE,timeout=30)
def digest(data):return hashlib.sha256(data).hexdigest()
evidence=json.loads((packet/'NATIVE_EVIDENCE.json').read_text(encoding='utf-8'))
assert evidence['head']==evidence['head_after']
assert evidence['working_patch_stable']
assert evidence['status_before']==evidence['status_after']
assert len(evidence['committed_source_files'])==38
assert len(evidence['working_source_files'])==5
assert len(evidence['protected'])==53 and all(row['unchanged'] for row in evidence['protected'])
for number in (1,2,6):
    issue=json.loads((packet/f'github-issue-{number}.json').read_text(encoding='utf-8'))
    assert issue['issue_number']==number and issue['url'].endswith('/issues/'+str(number))
assert len(re.findall(r'^## \d+\.',(packet/'README.md').read_text(encoding='utf-8'),re.M))==8

archives=[]
for label in ('CSV','JSONL'):
    record=json.loads((project/f'optimization/file-sql-20261101/{label}_CHECKPOINT.json').read_text(encoding='utf-8'))
    archive=Path(record['archive'])
    assert digest(archive.read_bytes())==record['archive_sha256']
    with zipfile.ZipFile(archive) as bundle:
        assert bundle.testzip() is None
        manifest=json.loads(bundle.read('MANIFEST.json'))
        for name,sha in manifest.items():assert digest(bundle.read(name))==sha,name
    protected_branch=record['protected_csv_branch'] if label=='CSV' else record['protected_branch']
    assert git('rev-parse',protected_branch).decode().strip()==record['feature_commit']
    archives.append({'label':label,'commit':record['feature_commit'],'archive':str(archive),'sha256':record['archive_sha256'],'members':record['archive_members'],'crc_and_sha256_reverified':True})
assert git('rev-parse','codex/fts-best-20261031').decode().strip()=='5bce9e06d1281347ad61344e4b01fab9158c58a2'

parked=['src/sql/parser/sql_parser_mysql_mode.y','src/sql/resolver/ddl/ob_explain_resolver.cpp','src/sql/rewrite/ob_transform_utils.cpp']
actual=git('diff','--name-only','--','src').decode().splitlines()
assert sorted(actual)==sorted(parked),actual
(packet/'native-snapshot/parked-windows-describe.patch').write_bytes(git('diff','--binary','--','src'))
parked_files=[{'path':name,'sha256_raw':digest((project/name).read_bytes())} for name in parked]
old_handoff=project/'handoff/fts-tuning/README.md'
log_presence={name:(project/'handoff/fts-tuning/logs'/name).is_file() for name in ('build.log','verify.log','test.log','notes.log')}
result={'verified_at':datetime.now(timezone(timedelta(hours=8))).isoformat(),
        'scope':'issue1 local coordination complete; issues2/6 implementation not accepted',
        'official_inputs_unchanged':{'manifest_entries':53,'duplicates_possible':True},
        'native_snapshot_head':evidence['head'],'native_snapshot_stable':True,
        'historical_archives':archives,'parked_source_files':parked_files,
        'old_fts_handoff_sha256':digest(old_handoff.read_bytes()),
        'old_fts_required_logs_present':log_presence,
        'runtime_tests_this_round':0,'native_builds_this_round':0,
        'native_writes_this_round':0,'remote_writes_this_round':0,
        'product_changes_this_round':0,'stop_after_delivery':True}
(packet/'VERIFICATION.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8',newline='\n')
print(json.dumps({'native_source_files':38,'pending_source_files':5,'protected_manifest_entries':'53/53','historical_archives_reverified':len(archives),'parked_source_files_preserved':3,'README_sections':8,'scope':'coordination only; no new runtime results','old_fts_required_logs_present':log_presence},indent=2))
