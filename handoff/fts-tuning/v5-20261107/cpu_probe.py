import json
from control_v2 import v,ROOT
from latency import measure
p,data,log=v.start('fixed','v5-cpu-aa')
record=[]
try:
    for repeat in range(3):
        row={'repeat':repeat,'workloads':{}}
        for name in ['select1','tokenize_ik','tokenize_beng']:
            row['workloads'][name]=measure(p.pid,name,1000)
            print('CPU_PROBE',repeat,name,{k:v for k,v in row['workloads'][name].items() if k!='latencies_ms'},flush=True)
        record.append(row)
        (ROOT/'cpu-aa.json').write_text(json.dumps(record,indent=2))
    (ROOT/'cpu-aa.complete.json').write_text(json.dumps({'completed':True,'rounds':len(record)}))
finally:v.stop(p,log)
