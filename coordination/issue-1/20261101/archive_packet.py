"""Archive the completed coordination packet and parked work, never product jobs."""
from datetime import datetime, timezone, timedelta
from pathlib import Path
import hashlib,json,subprocess,zipfile

packet=Path(__file__).resolve().parent
project=packet.parents[2]
phase=project/'optimization/file-sql-20261101'
def git(*args):return subprocess.check_output(['git','--no-optional-locks',*args],cwd=project,stderr=subprocess.PIPE,timeout=30)
def digest(data):return hashlib.sha256(data).hexdigest()
verification=json.loads((packet/'VERIFICATION.json').read_text(encoding='utf-8'))
for row in verification['parked_source_files']:
    assert digest((project/row['path']).read_bytes())==row['sha256_raw'],row['path']
assert digest((project/'handoff/fts-tuning/README.md').read_bytes())==verification['old_fts_handoff_sha256']
head=git('rev-parse','HEAD').decode().strip()
branch=git('branch','--show-current').decode().strip()
assert branch=='codex/issue-1-coordination-20261101'
assert not git('diff','HEAD','--','coordination/issue-1/20261101')
payload={}
for path in packet.rglob('*'):
    if path.is_file() and path.name!='CHECKPOINT.json' and '__pycache__' not in path.parts:
        payload[path.relative_to(project).as_posix()]=path.read_bytes()
for relative in ('STATE.json','SCOPE_HANDOFF.md','DESCRIBE_PROGRESS.md','describe-review.md','describe_checks.py','generated-and-wired-paths.json','describe-first-changed-sources.json','describe-first-release-build.log','describe-first-generate-parser.log'):
    path=phase/relative
    if path.is_file():payload[path.relative_to(project).as_posix()]=path.read_bytes()
for path in (phase/'sources-before-describe-first').rglob('*'):
    if path.is_file():payload[path.relative_to(project).as_posix()]=path.read_bytes()
for relative in ('docs/superpowers/plans/2026-11-01-file-sql-describe.md','optimization/FINAL_REPORT.md','optimization/BEST_RESULT.md','optimization/file-sql-20261101/CSV_CHECKPOINT.json','optimization/file-sql-20261101/JSONL_CHECKPOINT.json'):
    payload[relative]=(project/relative).read_bytes()
payload['COORDINATION.patch']=git('format-patch','-1',head,'--stdout')
record={'created_at':datetime.now(timezone(timedelta(hours=8))).isoformat(),
        'commit':head,'branch':branch,'scope':'issue1 coordination only',
        'status':'local coordination deliverable complete; waiting for user',
        'issues2_and6_accepted':False,'remote_issue_closed':False,
        'remote_writes':False,'product_build_or_sql_this_round':False,
        'pending_describe_work_preserved':True,'native_owner_workspace_untouched':True,
        'next_action':'Do not resume automatically; wait for explicit next task.'}
payload['CHECKPOINT.json']=(json.dumps(record,indent=2)+'\n').encode()
hashes={name:digest(data) for name,data in payload.items()}
payload['MANIFEST.json']=(json.dumps(hashes,indent=2,sort_keys=True)+'\n').encode()
output=project.parent/'artifacts/coordination';output.mkdir(parents=True,exist_ok=True)
archive=output/('issue1-checkpoint-'+datetime.now(timezone(timedelta(hours=8))).strftime('%Y%m%dT%H%M%S%z')+'.zip')
with zipfile.ZipFile(archive,'x',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as bundle:
    for name,data in payload.items():bundle.writestr(name,data)
with zipfile.ZipFile(archive) as bundle:
    assert bundle.testzip() is None
    for name,sha in hashes.items():assert digest(bundle.read(name))==sha
sha=digest(archive.read_bytes())
archive.with_suffix('.zip.sha256').write_text(sha+'  '+archive.name+'\n',encoding='utf-8')
record.update(archive=str(archive),sha256=sha,members=len(payload),bytes=archive.stat().st_size,crc_and_per_file_sha256_verified=True)
(packet/'CHECKPOINT.json').write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8',newline='\n')
print(json.dumps(record,indent=2))
