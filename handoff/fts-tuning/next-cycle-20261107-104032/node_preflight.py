from pathlib import Path
import hashlib,json,os,re,shutil,subprocess
r=Path('/home/xiazihan/benchmark/fts-v4-integration-20261106')
s=r/'src'
def command(args, timeout=15):
    p=subprocess.run(args,stdin=subprocess.DEVNULL,capture_output=True,text=True,timeout=timeout)
    return dict(rc=p.returncode,out=p.stdout.strip(),err=p.stderr.strip())
def digest(p):
    h=hashlib.sha256()
    with p.open('rb') as f:
        for b in iter(lambda:f.read(8<<20),b''): h.update(b)
    return h.hexdigest()
def status(*args):return command(list(args))
cc=s/'build_release/CMakeCache.txt'
cm=[line for line in cc.read_text().splitlines() if re.match(r'^(CMAKE_BUILD_TYPE:|CMAKE_CXX_COMPILER:|CMAKE_CXX_FLAGS_RELWITHDEBINFO:|CMAKE_PROJECT_INCLUDE:)',line)]
compilers=[]
for p in sorted(cc.parent.glob('CMakeFiles/*/CMakeCXXCompiler.cmake')):
    m=re.search(r'set\(CMAKE_CXX_COMPILER "([^"]+)"\)',p.read_text())
    if m and m[1] not in compilers:compilers.append(m[1])
tc=Path('/home/xiazihan/benchmark/seekdb-verify-20261104/toolchain/rustup/toolchains/1.98.1-x86_64-unknown-linux-gnu/bin')
mfile=r/'logs/source-fixed.json'
hist=json.loads(mfile.read_text())
extra=['src/storage/fts/ob_fts_stop_word.cpp','src/storage/fts/dict/ob_ft_range_dict.cpp','src/storage/fts/dict/ob_ft_dat_dict.cpp','src/storage/fts/dict/ob_ft_cache.h','src/storage/fts/dict/ob_ft_dict_hub.cpp','src/sql/engine/table/ob_table_scan_op.cpp','src/sql/engine/table/ob_table_scan_op.h','tools/benchmark/fts_large_bench.sh','tools/benchmark/fts_large_bench_gen.py','tools/benchmark/fts_large_bench_setup.sql','tools/benchmark/fts_large_bench_score.py','tools/benchmark/fts_large_bench_baseline.json']
d={'user':status('id','-un'),'os':status('uname','-srmo'),'guard':command(['python3',str(r/'resource_guard.py')],25),'mount':status('findmnt','-no','OPTIONS','--target',str(r)),'disk':status('df','-h',str(r)),'cmake':status('cmake','--version'),'cmake_cache':cm,
 'compilers':{p:status(p,'--version') for p in compilers},'rust':status(str(tc/'rustc'),'--version'),'cargo':status(str(tc/'cargo'),'--version'),
 'reference':str(r/'saved-binaries/seekdb-fixed'),'ref_sha256':digest(r/'saved-binaries/seekdb-fixed'),'source_manifest_sha256':digest(mfile),
 'sources':{f:digest(s/f) for f in sorted(set(hist)|set(extra))},
 'ports':status('ss','-ltnH','( sport = :2920 or sport = :2917 )'),
 'tmux':status('tmux','list-sessions'),
 'top_cpu':status('bash','-c','ps -eo pid,comm,stat,pcpu,pmem --sort=-pcpu | head -18'),
 'cpu_topology':status('lscpu','-e=CPU,CORE,SOCKET,NODE,ONLINE'),
 'cpus':sorted(os.sched_getaffinity(0)),'perf':status('perf','version') if shutil.which('perf') else {'available':False},
 'perf_event_paranoid':Path('/proc/sys/kernel/perf_event_paranoid').read_text().strip(),
 'loadavg':Path('/proc/loadavg').read_text().strip(),
 'client':str(s/'deps/3rd/u01/obclient/bin/obclient')}
d['source_mismatches_vs_sealed_manifest']=[f for f in hist if d['sources'][f]!=hist[f]]
print(json.dumps(d,indent=2))
