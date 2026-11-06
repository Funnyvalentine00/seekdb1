import json,threading,sys,subprocess
from control import v,ROOT
out={'protocol':'V5-AA-CONTROLLED-01','runs':[]}
for tag in ['v5-caa1-a','v5-caa1-b','v5-caa2-b','v5-caa2-a']:
    assert not (v.O/(tag+'.txt')).exists()
    p,data,log=v.start('fixed',tag)
    done=threading.Event();errors=[]
    def monitor():
        while not done.wait(30):
            q=subprocess.run([sys.executable,str(v.R/'resource_guard.py')],capture_output=True,text=True)
            print(q.stdout,flush=True)
            if q.returncode:errors.append(q.stdout+q.stderr);p.terminate();return
    t=threading.Thread(target=monitor);t.start()
    try:
        metrics=v.bench(tag,p.pid);assert not errors,errors
        out['runs'].append(dict(tag=tag,metrics=metrics,rss=v.rss(p.pid)))
        (ROOT/'controlled-aa-summary.json').write_text(json.dumps(out,indent=2))
    finally:done.set();t.join();v.stop(p,log)
(ROOT/'controlled-aa.complete.json').write_text(json.dumps({'tags':[r['tag'] for r in out['runs']]}))
print('CONTROLLED_AA_COMPLETE',flush=True)
