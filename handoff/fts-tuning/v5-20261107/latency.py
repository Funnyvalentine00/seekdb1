from pathlib import Path
import json,os,select,statistics,subprocess,time
from control_v2 import v,CLIENT_EXE,CLIENT_CPU

WORKLOADS={
 'select1':'SELECT 1;',
 'tokenize_ik':"SELECT tokenize('OceanBase是一款非常稳定的数据库，全文索引的分词器热路径优化包括解析器实例复用、停用词全局单例、内存池化数据结构等技术，在大量文档索引和查询场景下可以显著降低 CPU 开销。','ik');",
 'tokenize_beng':"SELECT tokenize('The quick brown fox jumps over the lazy dog. Full-text search indexing requires efficient tokenization on the hot path.','beng');",
 'query_cn':"SELECT COUNT(*) FROM fts_large_bench.docs WHERE MATCH(title, content) AGAINST('全文索引 分词器');",
 'query_beng':"SELECT COUNT(*) FROM fts_large_bench.docs WHERE MATCH(title_en, content_en) AGAINST('database performance audit');",
 'query_mixed':"SELECT COUNT(*) FROM fts_large_bench.docs WHERE MATCH(title, content) AGAINST('OceanBase 稳定 database');",
 'query_limit':"SELECT COUNT(*) FROM (SELECT id FROM fts_large_bench.docs WHERE MATCH(content) AGAINST('倒排索引 tokenizer') LIMIT 20) t;"}

def workers(pid):
    out={}
    for p in (Path('/proc')/str(pid)/'task').iterdir():
        try:
            if p.joinpath('comm').read_text().strip()=='ReqWorker':out[p.name]=int(p.joinpath('schedstat').read_text().split()[0])
        except FileNotFoundError:pass
    return out

def measure(pid,name,repetitions=1000):
    sql=WORKLOADS[name]
    cmd=['taskset','-c',str(CLIENT_CPU),CLIENT_EXE,'-h127.0.0.1',f'-P{v.PORT}','-uroot','-N','-s','-B','--raw','--unbuffered','--default-character-set=utf8mb4']
    p=subprocess.Popen(cmd,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    samples=[]
    def query(text):
        p.stdin.write((text+'\n').encode());p.stdin.flush()
        if not select.select([p.stdout],[],[],15)[0]:raise TimeoutError('No response: '+name)
        b=p.stdout.readline()
        if not b:raise RuntimeError('Client exited: '+p.stderr.read().decode(errors='replace'))
        return b
    try:
        query('SET NAMES utf8mb4; SELECT 1;')
        for _ in range(30):query(sql)
        before=workers(pid);assert before,'ReqWorker CPU accounting unavailable'
        for _ in range(repetitions):
            t=time.perf_counter_ns();query(sql);samples.append((time.perf_counter_ns()-t)/1e6)
        after=workers(pid)
    finally:
        p.stdin.close()
        try:p.wait(timeout=5)
        except subprocess.TimeoutExpired:p.terminate();p.wait(timeout=5)
    assert p.returncode==0
    same_workers=set(before)==set(after)
    quantiles=statistics.quantiles(samples,n=100,method='inclusive')
    return dict(n=len(samples),latencies_ms=samples,mean_ms=statistics.mean(samples),median_ms=statistics.median(samples),p95_ms=quantiles[94],p99_ms=quantiles[98],same_workers=same_workers,worker_count=len(after),worker_cpu_ns_per_query=(sum(after.values())-sum(before.values()))/repetitions if same_workers else None)
