from pathlib import Path
import concurrent.futures,hashlib,json,sys
from control_v2 import v,ROOT
variant=sys.argv[1];label=sys.argv[2]
assert not (ROOT/(label+'-check.json')).exists()
p,data,log=v.start(variant,'v5-'+label+'-check')

def result(q):
    b=v.sql(q)
    return dict(rc=b.returncode,stdout_sha256=hashlib.sha256(b.stdout).hexdigest(),stderr_sha256=hashlib.sha256(b.stderr).hexdigest()),b

out={}
try:
    v.bench('v5-'+label+'-check-load',p.pid)
    out['semantics']=v.capture_semantics(label)
    out['edges']=v.edge_probes(label,p.pid)
    cases=(v.R/'tokenize.sql').read_text().splitlines()
    out['tokenize']=[]
    for i,q in enumerate(cases[1:],2):
        record,b=result(cases[0]+'\n'+q)
        (v.O/f'{label}-tokenize-{i}.out').write_bytes(b.stdout);(v.O/f'{label}-tokenize-{i}.err').write_bytes(b.stderr)
        out['tokenize'].append(dict(line=i,**record))
    lines=(ROOT/'v5-word-aggregation.sql').read_text().splitlines()
    split=next(i for i,line in enumerate(lines) if line.startswith('DROP DATABASE'))
    out['aggregation']=[]
    for i,line in enumerate(lines[1:split],2):
        record,b=result(lines[0]+'\n'+line)
        (v.O/f'{label}-aggregate-{i}.out').write_bytes(b.stdout);(v.O/f'{label}-aggregate-{i}.err').write_bytes(b.stderr)
        out['aggregation'].append(dict(line=i,**record))
    out['dml'],b=result(lines[0]+'\n'+'\n'.join(lines[split:]))
    (v.O/f'{label}-dml.out').write_bytes(b.stdout);(v.O/f'{label}-dml.err').write_bytes(b.stderr)
    assert b.returncode==0,b.stderr
    queries=["SET NAMES utf8mb4; SELECT tokenize('resume résumé resumé RESUME résumé','beng');", "SET NAMES utf8mb4; SELECT tokenize('数据库 database ＡＢＣ abc 数据库','ik');"]
    reference={q:result(q)[0] for q in queries}
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        records=list(pool.map(lambda i:(queries[i%2],result(queries[i%2])[0]),range(80)))
    assert all(ref==reference[q] for q,ref in records)
    out['concurrency']={'requests':80,'workers':4,'reference':reference,'all_match':True}
    out['regression']={name:v.suite(label,data,name) for name in ['fts_index','file_sql','ai_column']}
    (ROOT/(label+'-check.json')).write_text(json.dumps(out,indent=2,ensure_ascii=False))
    print('CHECK_COMPLETE',label,flush=True)
finally:v.stop(p,log)
