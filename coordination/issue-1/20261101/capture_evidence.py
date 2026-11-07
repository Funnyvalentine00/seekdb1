"""Read-only capture of another owner's checkout; write only this packet."""
from datetime import datetime, timezone, timedelta
from pathlib import Path
import hashlib,json,subprocess

packet=Path(__file__).resolve().parent
project=packet.parents[2]
native=Path('/opt/codex-seekdb-20261030')
target=packet/'native-snapshot'
target.mkdir(exist_ok=False)
baseline='5bce9e06d1281347ad61344e4b01fab9158c58a2'

def git(*args):
    return subprocess.check_output(['git','--no-optional-locks',*args],cwd=native,stderr=subprocess.PIPE,timeout=30)

head=git('rev-parse','HEAD').decode().strip()
status=git('status','--short','--untracked-files=no').decode()
working=git('diff','--binary',head,'--','src')
paths=git('diff','--name-only',baseline,head,'--','src').decode().splitlines()
records=[]
for relative in paths:
    data=git('show',head+':'+relative)
    records.append({'path':relative,'sha256':hashlib.sha256(data).hexdigest(),'bytes':len(data)})
    # Keep focused text for contract review, not copies of large generated files.
    if len(data)<250000 and ('system_variable' not in relative):
        destination=target/'committed'/relative
        destination.parent.mkdir(parents=True,exist_ok=True)
        destination.write_bytes(data)

changed=git('diff','--name-only',head,'--','src').decode().splitlines()
for relative in changed:
    destination=target/'working'/relative
    destination.parent.mkdir(parents=True,exist_ok=True)
    destination.write_bytes((native/relative).read_bytes())
(target/'working.patch').write_bytes(working)
(target/'committed.patch').write_bytes(git('diff','--binary',baseline,head,'--','src'))
(target/'history.txt').write_bytes(git('log','-6','--format=%H %ai %s'))
handoff=[]
for relative in ('README.md','changes.patch','logs/build.log','logs/verify.log','logs/test.log','logs/notes.log'):
    path=native/'handoff/file-sql'/relative
    record={'path':'handoff/file-sql/'+relative,'exists':path.is_file()}
    if path.is_file():
        data=path.read_bytes()
        destination=target/'handoff/file-sql'/relative
        destination.parent.mkdir(parents=True,exist_ok=True)
        destination.write_bytes(data)
        record.update(bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),stable_during_copy=(data==path.read_bytes()))
    handoff.append(record)

protected=[]
for relative in ('optimization/protected-inputs.sha256','optimization/file-sql-baseline-20261101/protected-inputs.sha256'):
    for line in (project/relative).read_text().splitlines():
        if not line.strip():continue
        expected,name=line.split(maxsplit=1);name=name.lstrip(' *')
        path=native/name
        actual=hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None
        protected.append({'manifest':relative,'path':name,'expected':expected,'actual':actual,'unchanged':actual==expected})

record={'captured_at':datetime.now(timezone(timedelta(hours=8))).isoformat(),
        'mode':'read-only; no SQL, builds, restarts, native writes, fetches or pushes',
        'native':str(native),'baseline':baseline,'head':head,
        'branch':git('branch','--show-current').decode().strip(),
        'status_before':status,'status_after':git('status','--short','--untracked-files=no').decode(),
        'head_after':git('rev-parse','HEAD').decode().strip(),
        'working_patch_stable':working==git('diff','--binary',head,'--','src'),
        'committed_source_files':records,'working_source_files':changed,
        'handoff':handoff,'protected':protected}
(packet/'NATIVE_EVIDENCE.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps({key:record[key] for key in ('captured_at','head','branch','status_before','head_after','working_patch_stable')},indent=2))
print('Protected checks:',sum(row['unchanged'] for row in protected),'/',len(protected),'manifest entries; duplicates possible')
