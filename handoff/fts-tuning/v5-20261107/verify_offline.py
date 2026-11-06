"""Verify the frozen V5 evidence without starting servers or rewriting results."""
from pathlib import Path
import hashlib,json,math,re,statistics,tarfile
R=Path(__file__).resolve().parent
for name,record in json.loads((R/'archive-manifest.json').read_text()).items():
    data=(R/name).read_bytes()
    assert len(data)==record['bytes'] and hashlib.sha256(data).hexdigest()==record['sha256'],name
with tarfile.open(R/'final-evidence.tar.gz') as t:
    members=t.getmembers()
    assert all(m.isfile() and not m.name.startswith('/') and '..' not in Path(m.name).parts for m in members)
    def load(name):return json.loads(t.extractfile(name).read())
    def text(name):return t.extractfile(name).read().decode()
    checks=[load(name+'-check.json') for name in ['e01-before','e01-after','final-reference']]
    for key in ['semantics','tokenize','aggregation','dml','concurrency']:
        assert checks[0][key]==checks[1][key]==checks[2][key],key
    for check in checks:
        assert len(check['edges'])==9 and all(row['rc']==0 for row in check['edges'].values())
        assert len(check['tokenize'])==22 and len(check['aggregation'])==12
        assert check['concurrency']['requests']==80 and check['concurrency']['all_match']
        for suite,expected in [('fts_index',{'basic_dml','partitioned_simple_query'}),('file_sql',set()),('ai_column',set())]:
            assert {row['case'] for row in check['regression'][suite] if row['rc']}==expected
    for case in checks[0]['edges']:
        for key in ['rc','stdout_sha256','stderr_sha256']:
            assert len({check['edges'][case][key] for check in checks})==1
    reports=[]
    for m in members:
        name=m.name
        if not name.startswith('results/v5-') or not name.endswith('.txt') or '/' in name[len('results/'):]:continue
        metrics={v[1]:float(v[2]) for v in re.finditer(r'^([A-Za-z0-9_]+):\s*([0-9.]+)\s*$',text(name),re.M)}
        if 'query_cn_hits' not in metrics:continue
        assert [metrics[k] for k in ['query_cn_hits','query_beng_hits','query_mixed_hits','query_limit_hits']]==[8001,11000,7332,20]
        assert [metrics[k] for k in ['rows','batch','rounds','query_rounds','samples','warmup','skip_load']]==[20000,500,3000,200,3,30,0]
        reports.append(name)
    assert len(reports)==17,len(reports)
    ab=load('e01-ab.json');assert len(ab['pairs'])==3
    analysis=json.loads((R/'e01-analysis.json').read_text())
    for key,row in analysis['wall'].items():
        improvements=[100*(1-p['e01']['metrics'][key]/p['fixed']['metrics'][key]) for p in ab['pairs']]
        assert abs(statistics.median(improvements)-row['median_improvement_pct'])<1e-10,key
    latency_samples=0
    for m in members:
        if m.name.endswith('-latencies.json'):
            rows=load(m.name)
            latency_samples+=sum(len(row['latencies_ms']) for row in rows.values())
    assert latency_samples==42000,latency_samples
    cpu=load('cpu-aa.json');assert sum(len(row['latencies_ms']) for block in cpu for row in block['workloads'].values())==9000
    scores=[m.name for m in members if m.name.startswith('results/scores/') and m.name.endswith('.txt')]
    assert len(scores)==7 and all('score: 100.00 / 100' in text(name) for name in scores)
    assert load('e01-build.json')['rc']==0
    assert load('e01-restored.json')['restored_sha256']==load('e01-source.json')['source_before_sha256']
state=json.loads((R/'FINAL_STATE.json').read_text());budget=json.loads((R/'BUDGET.json').read_text())
assert state['full_acceptance'] is False and state['new_product_source_changes_retained'] is False
assert budget['total_experiments']==8 and budget['implemented']==1 and budget['failed']==1 and budget['success']==0
assert budget['formal_ab_pairs']=={'E01':3} and budget['implementation_corrections']=={'E01':0}
assert budget['final_phase_fraction']>=.2
print('PASS: 3 archive hashes; 17 exact-input/hit reports; 3 formal pairs; 42000 tail + 9000 calibration samples; before/candidate/final functional parity; 57 passes/6 known failures; no promotion; budget/20% closeout preserved.')
