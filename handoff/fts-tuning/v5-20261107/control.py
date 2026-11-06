from pathlib import Path
import importlib.util,json,os,socket,subprocess,sys,time
sys.path.insert(0,'/home/xiazihan/benchmark/fts-v4-integration-20261106')
import verify_linux as v
ROOT=v.R/'v5';v.O=ROOT/'results';v.PORT=2920
allowed=sorted(os.sched_getaffinity(0));assert len(allowed)>=5,allowed
SERVER_CPUS=allowed[:4];CLIENT_CPU=allowed[4]
CLIENT_EXE=str(v.CLIENT/'obclient')
v.M=f'taskset -c {CLIENT_CPU} {CLIENT_EXE}'

def sql(query,force=False):
    return subprocess.run(['taskset','-c',str(CLIENT_CPU),CLIENT_EXE,'-h127.0.0.1',f'-P{v.PORT}','-uroot','-N','-s','-B','--default-character-set=utf8mb4']+(['--force'] if force else [])+['-e',query],capture_output=True,timeout=180,env=v.E)
v.sql=sql

def stop(p,log):
    if p.poll() is None:
        p.terminate()
        try:p.wait(timeout=40)
        except subprocess.TimeoutExpired:p.kill();p.wait()
    log.close()
    print('OWN_DATA_PRESERVED',p.args[p.args.index('--base-dir')+1],flush=True)
v.stop=stop

def start(variant,tag):
    subprocess.run([sys.executable,str(v.R/'resource_guard.py')],check=True)
    with socket.socket() as s:s.bind(('127.0.0.1',v.PORT))
    data=v.R/'data'/tag;data.mkdir(exist_ok=False);(data/'.owned-v4-validation').write_text(tag)
    log=(v.O/(tag+'-server.log')).open('x')
    binary= v.R/'saved-binaries/seekdb-fixed' if variant=='fixed' else ROOT/'saved-binaries'/('seekdb-'+variant)
    cmd=['taskset','-c',','.join(map(str,SERVER_CPUS)),str(binary),'--nodaemon','--port',str(v.PORT),'--base-dir',str(data),
         '--parameter','datafile_size=2G','--parameter','datafile_maxsize=2G','--parameter','memory_budget=2G',
         '--parameter','vector_memory_limit=512M','--parameter','log_disk_size=2G','--parameter','cpu_count=4']
    p=subprocess.Popen(cmd,cwd=data,env=v.E,stdout=log,stderr=subprocess.STDOUT)
    try:
        for _ in range(240):
            if p.poll() is not None:raise RuntimeError('server exited '+tag)
            if sql('select 1').returncode==0:
                (v.O/(tag+'-runtime.json')).write_text(json.dumps(dict(server_cpus=SERVER_CPUS,client_cpu=CLIENT_CPU,cpu_count=4,memory_budget='2G',binary=str(binary),pid=p.pid,server_affinity=sorted(os.sched_getaffinity(p.pid))),indent=2))
                return p,data,log
            time.sleep(.5)
        raise RuntimeError('startup timeout '+tag)
    except BaseException:stop(p,log);raise
v.start=start
