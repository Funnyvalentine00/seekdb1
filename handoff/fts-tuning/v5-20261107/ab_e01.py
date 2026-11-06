import hashlib,json,subprocess,sys,threading
from control_v2 import v,ROOT
from latency import measure
before=json.loads((ROOT/'e01-before-check.json').read_text());after=json.loads((ROOT/'e01-after-check.json').read_text())
mismatches=[]
for section in ['semantics','tokenize','aggregation','dml','concurrency']:
    if before[section]!=after[section]:mismatches.append(section)
for case,record in before['edges'].items():
    for key in ['rc','stdout_sha256','stderr_sha256']:
        if record[key]!=after['edges'][case][key]:mismatches.append('edge '+case+' '+key)
for suite in before['regression']:
    b={r['case']:r['rc'] for r in before['regression'][suite]};a={r['case']:r['rc'] for r in after['regression'][suite]}
    if b!=a:mismatches.append('regression '+suite)
(ROOT/'e01-correctness-comparison.json').write_text(json.dumps({'mismatches':mismatches,'no_new_failures':not mismatches},indent=2))
assert not mismatches,mismatches
record={'experiment':'E01','pairs':[],'order':[['fixed','e01'],['e01','fixed'],['fixed','e01']],'max_formal_pairs':3}
for i,order in enumerate(record['order'],1):
    row={}
    for variant in order:
        tag=f'v5-e01-p{i}-{variant}'
        assert not (v.O/(tag+'.txt')).exists(),'Never overwrite'
        p,data,log=v.start(variant,tag)
        done=threading.Event();errors=[]
        def watch():
            while not done.wait(30):
                q=subprocess.run([sys.executable,str(v.R/'resource_guard.py')],capture_output=True,text=True)
                print(q.stdout,flush=True)
                if q.returncode:errors.append(q.stdout+q.stderr);p.terminate();return
        thread=threading.Thread(target=watch);thread.start()
        try:
            metrics=v.bench(tag,p.pid)
            rss=v.rss(p.pid)
            latency={name:measure(p.pid,name,1000) for name in ['select1','tokenize_ik','tokenize_beng','query_cn','query_beng','query_mixed','query_limit']}
            assert not errors,errors
            (v.O/(tag+'-latencies.json')).write_text(json.dumps(latency,indent=2))
            row[variant]={'tag':tag,'metrics':metrics,'rss_benchmark':rss,'rss_after_latency':v.rss(p.pid),'tail':{name:{key:value for key,value in d.items() if key!='latencies_ms'} for name,d in latency.items()}}
            (ROOT/(tag+'-complete.json')).write_text(json.dumps(row[variant],indent=2))
        finally:done.set();thread.join();v.stop(p,log)
    record['pairs'].append(row)
    (ROOT/'e01-ab.json').write_text(json.dumps(record,indent=2))
(ROOT/'e01-ab.complete.json').write_text(json.dumps({'pairs':len(record['pairs'])}))
print('E01_AB_COMPLETE',flush=True)
