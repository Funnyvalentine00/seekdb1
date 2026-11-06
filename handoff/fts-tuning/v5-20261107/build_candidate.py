from pathlib import Path
import hashlib,json,os,shutil,signal,subprocess,time
R=Path('/home/xiazihan/benchmark/fts-v4-integration-20261106');V=R/'v5';S=R/'src'
meta=json.loads((V/'e01-source.json').read_text());target=S/meta['files'][0]
assert hashlib.sha256(target.read_bytes()).hexdigest()==meta['source_before_sha256']
assert not (V/'e01-build.json').exists()
subprocess.run(['python3',str(R/'resource_guard.py')],check=True)
subprocess.run(['python3',str(R/'validate_source.py'),'fixed'],cwd=S,check=True)
(V/'e01-preimage.txt').write_bytes(target.read_bytes())
subprocess.run(['patch','--dry-run','-p1','-i',str(V/'e01.patch')],cwd=S,check=True)
subprocess.run(['patch','-p1','-i',str(V/'e01.patch')],cwd=S,check=True)
assert hashlib.sha256(target.read_bytes()).hexdigest()==meta['source_after_sha256']
env=dict(os.environ,RUSTUP_HOME='/home/xiazihan/benchmark/seekdb-verify-20261104/toolchain/rustup',CARGO_HOME='/home/xiazihan/benchmark/seekdb-verify-20261104/toolchain/cargo',CARGO_NET_OFFLINE='true',CARGO_BUILD_JOBS='2',CMAKE_BUILD_PARALLEL_LEVEL='2')
env['PATH']=env['CARGO_HOME']+'/bin:'+env['PATH']
cmd=['bash','build.sh','release','-DCMAKE_PROJECT_INCLUDE='+str(R/'conservative_link.cmake'),'--make','-j2']
started=time.time()
with (V/'logs/e01-build.log').open('x') as log:
    p=subprocess.Popen(cmd,cwd=S,env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
    while p.poll() is None:
        time.sleep(10)
        q=subprocess.run(['python3',str(R/'resource_guard.py')],capture_output=True,text=True)
        print(q.stdout,flush=True)
        if q.returncode:
            os.killpg(p.pid,signal.SIGTERM);p.wait();raise RuntimeError('resource guard failed '+q.stdout+q.stderr)
    record={'rc':p.returncode,'command':cmd,'elapsed_seconds':time.time()-started,'source':meta}
(V/'e01-build.json').write_text(json.dumps(record,indent=2))
assert p.returncode==0,record
(V/'saved-binaries').mkdir(exist_ok=True)
binary=V/'saved-binaries/seekdb-e01';assert not binary.exists()
shutil.copyfile(S/'build_release/src/observer/seekdb',binary);binary.chmod(0o755)
h=hashlib.sha256()
with binary.open('rb') as f:
    for b in iter(lambda:f.read(8*1024*1024),b''):h.update(b)
record['binary_sha256']=h.hexdigest();record['binary_bytes']=binary.stat().st_size
record['cmake_config']=[x for x in (S/'build_release/CMakeCache.txt').read_text().splitlines() if any(k in x for k in ['CMAKE_BUILD_TYPE:','CMAKE_CXX_FLAGS_RELWITHDEBINFO:','CMAKE_PROJECT_INCLUDE:'])]
record['related_source_sha256']={path:hashlib.sha256((S/path).read_bytes()).hexdigest() for path in json.loads((R/'logs/source-fixed.json').read_text())}
record['related_source_sha256'][meta['files'][0]]=hashlib.sha256(target.read_bytes()).hexdigest()
(V/'e01-build.json').write_text(json.dumps(record,indent=2))
(V/'e01-build.complete.json').write_text(json.dumps({'binary_sha256':h.hexdigest(),'source_after_sha256':meta['source_after_sha256']}))
print('E01_BUILD_COMPLETE',record['binary_sha256'],flush=True)
