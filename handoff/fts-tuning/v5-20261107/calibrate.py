from pathlib import Path
import hashlib,json,os,socket,subprocess,sys,threading,time
sys.path.insert(0,'/home/xiazihan/benchmark/fts-v4-integration-20261106')
import verify_linux as v
ROOT=v.R/'v5'
v.O=ROOT/'results';v.O.mkdir(parents=True,exist_ok=True)
v.PORT=2920

def stop_owned(p,log):
    if p.poll() is None:
        p.terminate()
        try:p.wait(timeout=40)
        except subprocess.TimeoutExpired:p.kill();p.wait()
    log.close()
    print('OWN_DATA_PRESERVED',p.args[p.args.index('--base-dir')+1],flush=True)
v.stop=stop_owned

manifest=json.loads((v.R/'logs/build.complete.json').read_text())
p=v.R/'saved-binaries/seekdb-fixed'
h=hashlib.sha256()
with p.open('rb') as f:
    for chunk in iter(lambda:f.read(8*1024*1024),b''):h.update(chunk)
assert h.hexdigest()==manifest['fixed']
with socket.socket() as s:s.bind(('127.0.0.1',v.PORT))
record={'binary_sha256':h.hexdigest(),'runs':[]}
for tag in ['v5-aa1-a','v5-aa1-b','v5-aa2-b','v5-aa2-a']:
    assert not (v.O/(tag+'.txt')).exists(),'Never overwrite evidence'
    p,data,log=v.start('fixed',tag)
    done=threading.Event();errors=[]
    def watch():
        while not done.wait(30):
            q=subprocess.run([sys.executable,str(v.R/'resource_guard.py')],capture_output=True,text=True)
            print(q.stdout,flush=True)
            if q.returncode:
                errors.append(q.stdout+q.stderr);p.terminate();return
    t=threading.Thread(target=watch);t.start()
    try:
        metrics=v.bench(tag,p.pid)
        assert not errors,errors
        record['runs'].append({'tag':tag,'metrics':metrics,'rss':v.rss(p.pid)})
        (ROOT/'aa-summary.json').write_text(json.dumps(record,indent=2))
    finally:
        done.set();t.join();v.stop(p,log)
(ROOT/'aa.complete.json').write_text(json.dumps({'tags':[r['tag'] for r in record['runs']],'binary_sha256':record['binary_sha256']}))
print('AA_COMPLETE',flush=True)
