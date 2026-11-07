from pathlib import Path
import json,os,re,shutil,subprocess,time
r=Path('/home/xiazihan/benchmark/fts-v4-integration-20261106')
def cmd(args, timeout=12):
    try:
        p=subprocess.run(args,stdin=subprocess.DEVNULL,capture_output=True,text=True,timeout=timeout)
        return dict(rc=p.returncode,out=p.stdout.strip(),err=p.stderr.strip())
    except (OSError, subprocess.TimeoutExpired) as e:
        return dict(error=str(e))
def cpu():
    return {int(s[3:].split()[0]):[int(v) for v in s.split()[1:9]] for s in Path('/proc/stat').read_text().splitlines() if re.match(r'^cpu\d+\s',s)}
def diff(a,b):
    return {k:[y-x for x,y in zip(a[k],b[k])] for k in a if k in b}
samples=[]
prev=cpu()
for _ in range(4):
    time.sleep(1)
    now=cpu()
    samples.append(diff(prev,now))
    prev=now
tot={i:sum(sum(s[i][:3]+s[i][5:]) for s in samples) for i in sorted(os.sched_getaffinity(0))}
irq={i:sum(sum(s[i][5:7]) for s in samples) for i in tot}
perfs=[]
for root in [Path('/usr/lib/linux-tools'),Path('/usr/local/bin'),Path('/home/xiazihan/bin')]:
    for p in list(root.glob('*/perf'))+list(root.glob('perf')):
        if p.is_file() and os.access(p,os.X_OK) and str(p) not in perfs:perfs.append(str(p))
d={'sample_seconds':4,'cpu_ticks_delta':samples,'busy_ticks':tot,'irq_softirq_ticks':irq,'perf_trials':{p:cmd([p,'stat','-e','task-clock:u,instructions:u','--','/bin/true']) for p in perfs},'gdb':shutil.which('gdb'),'bpftrace':shutil.which('bpftrace')}
ps=Path('/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe')
d['host_load']=cmd([str(ps),'-NoProfile','-Command',"(Get-CimInstance Win32_Processor | Select-Object LoadPercentage,CurrentClockSpeed,MaxClockSpeed | ConvertTo-Json -Compress)"])
logs=[]
for data in sorted((r/'data').glob('v5-*')):
    if data.name not in ['v5-final-reference-check','v5-caa1a','v5-caa1b']: continue
    for p in sorted((data/'log').glob('observer.log*'))[-4:]:
        if not p.is_file() or p.is_symlink(): continue
        marker=[]
        with p.open('rb') as f:
            for line in f:
                if b'build_ranges_concurrently_thread_pool completed' in line or b'build dat finished' in line:
                    marker.append(line[:700].decode(errors='replace').strip())
                if len(marker)>=32: break
        if marker: logs.append({'data':data.name,'log':p.name,'dictionary_events':marker})
d['historic_dictionary_events']=logs
print(json.dumps(d,indent=2))
