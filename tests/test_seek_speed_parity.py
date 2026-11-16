"""Compare real legacy Kotlin timeline to the synthetic C++ replay fixture."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT.parent
DEFAULT_LEGACY_REPO = Path(r'D:\Bilibili_Innocent_Lab')
DEFAULT_GRADLE_CACHE = Path(r'D:\Gradle\caches\modules-2\files-2.1')
DEFAULT_JAVA = Path(r'C:\Program Files\Android\Android Studio\jbr\bin\java.exe')
LEGACY_RELATIVE = Path('Bilibili_Innocent_Lab/app/src/main/java/com/Bilibili_Innocent_Lab/xposedmodule/hook/feature/interpolation')

def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def jar(cache, group, name, version):
    candidates=sorted((cache/group/name/version).rglob(name+'-'+version+'.jar'))
    if len(candidates)!=1: raise RuntimeError('missing or ambiguous jar: '+name)
    return candidates[0]

def main():
    p=argparse.ArgumentParser();p.add_argument('--exe',type=Path,required=True);p.add_argument('--out-dir',type=Path,required=True)
    p.add_argument('--legacy-repo',type=Path,default=DEFAULT_LEGACY_REPO,help='repository containing the frozen BIL baseline')
    p.add_argument('--gradle-cache',type=Path,default=DEFAULT_GRADLE_CACHE,help='Gradle modules-2/files-2.1 directory')
    p.add_argument('--java',type=Path,default=DEFAULT_JAVA,help='Java executable for Kotlin compilation and execution')
    a=p.parse_args()
    exe=a.exe.resolve(strict=True);legacy_repo=a.legacy_repo.resolve(strict=True);cache=a.gradle_cache.resolve(strict=True);java=a.java.resolve(strict=True)
    if not exe.is_file() or not legacy_repo.is_dir() or not cache.is_dir() or not java.is_file():raise ValueError('invalid input type')
    legacy=legacy_repo/LEGACY_RELATIVE
    source=legacy/'FrameTimeline.kt';rate=legacy/'FrameRateTarget.kt';oracle=ROOT/'tests/oracle/SeekSpeedOracle.kt';fixture=ROOT/'tests/fixtures/timeline/seek_speed.jsonl'
    baseline_path=ROOT/'artifacts/M0/source-inventory.json';baseline=json.loads(baseline_path.read_text())
    expected={x['path'].replace('\\','/').rsplit('/',1)[-1]:x['sha256'] for x in baseline['files']}
    if sha(source)!=expected[source.name] or sha(rate)!=expected[rate.name]: raise RuntimeError('legacy baseline differs')
    compiler=jar(cache,'org.jetbrains.kotlin','kotlin-compiler-embeddable','2.4.21');stdlib=jar(cache,'org.jetbrains.kotlin','kotlin-stdlib','2.4.21')
    script=jar(cache,'org.jetbrains.kotlin','kotlin-script-runtime','2.4.21');ann=jar(cache,'org.jetbrains','annotations','13.0')
    coroutines=jar(cache,'org.jetbrains.kotlinx','kotlinx-coroutines-core-jvm','1.10.2')
    out=a.out_dir.absolute();out.mkdir(parents=True,exist_ok=False)
    inputs=[source,rate,oracle,fixture,exe,java,compiler,stdlib,script,ann,coroutines,baseline_path,Path(__file__).resolve()]
    before={str(x):sha(x) for x in inputs};records=[]
    class CommandStartError(RuntimeError): pass
    class CommandTimeoutError(RuntimeError): pass
    def save_records(): (out/'commands.json').write_text(json.dumps(records,indent=2)+'\n')
    def run(name,argv):
        argv=list(map(str,argv));entry={'name':name,'argv':argv,'state':'ATTEMPTING','process_started':None};records.append(entry);save_records()
        try:process=subprocess.Popen(argv,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        except OSError as error:
            stderr=(type(error).__name__+': '+str(error)+'\n').encode('utf-8',errors='replace')
            (out/(name+'.stdout')).write_bytes(b'');(out/(name+'.stderr')).write_bytes(stderr)
            entry.update(state='START_FAILED',process_started=False,error_type=type(error).__name__,
                stdout_sha256=hashlib.sha256(b'').hexdigest(),stderr_sha256=hashlib.sha256(stderr).hexdigest());save_records()
            raise CommandStartError(name+' could not start') from error
        entry.update(state='RUNNING',process_started=True)
        try:save_records()
        except BaseException:
            process.kill();process.communicate();raise
        try:stdout,stderr=process.communicate(timeout=120)
        except subprocess.TimeoutExpired as error:
            process.kill();stdout,stderr=process.communicate()
            (out/(name+'.stdout')).write_bytes(stdout);(out/(name+'.stderr')).write_bytes(stderr)
            entry.update(state='TIMED_OUT',exit_code=process.returncode,stdout_sha256=hashlib.sha256(stdout).hexdigest(),
                stderr_sha256=hashlib.sha256(stderr).hexdigest());save_records()
            raise CommandTimeoutError(name+' timed out') from error
        except BaseException as error:
            if process.poll() is None:process.kill()
            process.wait()
            entry.update(state='PROCESS_IO_FAILED',exit_code=process.returncode,error_type=type(error).__name__);save_records()
            raise
        (out/(name+'.stdout')).write_bytes(stdout);(out/(name+'.stderr')).write_bytes(stderr)
        entry.update(state='COMPLETED',exit_code=process.returncode,
            stdout_sha256=hashlib.sha256(stdout).hexdigest(),stderr_sha256=hashlib.sha256(stderr).hexdigest());save_records()
        return subprocess.CompletedProcess(argv,process.returncode,stdout,stderr)
    try:
        git_blobs={}
        for original in (source,rate):
            git_path='Bilibili_Innocent_Lab/app/src/main/java/com/Bilibili_Innocent_Lab/xposedmodule/hook/feature/interpolation/'+original.name
            q=run('git-show-'+original.stem,['git','-c','safe.directory='+str(legacy_repo).replace('\\','/'),'-C',str(legacy_repo),'show',baseline['source_commit']+':'+git_path])
            if q.returncode:raise RuntimeError('legacy commit lookup failed')
            blob=q.stdout
            if blob.replace(b'\r\n',b'\n')!=original.read_bytes().replace(b'\r\n',b'\n'):raise RuntimeError('legacy commit content differs')
            git_blobs[original.name]=hashlib.sha256(blob).hexdigest()
        compiled=out/'legacy-oracle.jar';cp=os.pathsep.join(map(str,[compiler,stdlib,script,ann,coroutines]))
        q=run('compile',[java,'-cp',cp,'org.jetbrains.kotlin.cli.jvm.K2JVMCompiler','-no-stdlib','-no-reflect','-classpath',os.pathsep.join(map(str,[stdlib,ann])),'-d',compiled,source,rate,oracle])
        if q.returncode: raise RuntimeError('Kotlin compile failed')
        def legacy(label,path):
            q=run(label,[java,'-cp',str(compiled)+os.pathsep+str(stdlib),'com.Bilibili_Innocent_Lab.xposedmodule.hook.feature.interpolation.SeekSpeedOracleKt',path])
            if q.returncode: raise RuntimeError(label+' failed')
            return [line.split('|') for line in q.stdout.decode('utf-8').splitlines()]
        def cpp(label,path,ok):
            target=out/(label+'.jsonl');q=run(label,[exe,'--events',path,'--out',target])
            if (q.returncode==0)!=ok or target.exists()!=ok: raise RuntimeError(label+' unexpected status')
            if not ok:
                if b'no proposal to commit' not in q.stderr: raise RuntimeError(label+' wrong failure')
                return None
            return [json.loads(x) for x in target.read_text().splitlines()]
        old=legacy('legacy',fixture);new=cpp('cpp',fixture,True)
        if len(old)!=len(new):raise RuntimeError('event count mismatch')
        def ids(value):return [] if value=='' else [int(x) for x in value.split(',')]
        for a,b in zip(old,new):
            seq=int(a[0]);kind=a[1]
            if seq!=b['seq']:raise RuntimeError('sequence mismatch')
            if kind=='config': continue
            if kind=='begin': good=int(a[2])==b['generation'] and ids(a[3])==b['discarded']
            elif kind=='clock': good=int(a[2])==b['clock_state'] and ids(a[3])==b['discarded']
            elif kind=='offer': good=int(a[2])==b['accepted'] and ids(a[3])==b['discarded']
            elif kind=='ready': good=(b['waiting_reason']==0 and [int(x) for x in a[2:6]]==[b['due_ns'],b['media_us'],b['before_id'],b['after_id']] and abs(float(a[6])-b['alpha'])<1e-12 and int(a[7])==b['mode'])
            elif kind=='wait': good=[int(x) for x in a[2:]]==[b['waiting_reason'],b['due_ns'],b['media_us']]
            elif kind=='commit': good=int(a[2])==b['status']
            elif kind=='close': good=ids(a[2])==b['discarded']
            else: raise RuntimeError('unknown oracle row')
            if not good:raise RuntimeError('semantic mismatch seq '+str(seq))
        rows=[json.loads(x) for x in fixture.read_text().splitlines()]
        for label,prefix,clock,now in [('speed_pending',8,9,16666666),('seek_pending',17,18,33333332)]:
            subset=rows[:prefix]+[dict(rows[clock]),{'op':'commit','now_ns':now}]
            for i,row in enumerate(subset,1):row['seq']=i
            path=out/(label+'-input.jsonl');path.write_text(''.join(json.dumps(x,separators=(',',':'))+'\n' for x in subset),encoding='ascii')
            old=legacy('legacy-'+label,path)
            if old[-1][1:]!=['commit','-1']:raise RuntimeError(label+' legacy proposal accepted')
            cpp('cpp-'+label,path,False)
        after={str(x):sha(x) for x in inputs}
        if before!=after:raise RuntimeError('input drift')
        (out/'summary.json').write_text(json.dumps({'state':'PASS','scope':'SYNTHETIC_HOST_LEGACY_KOTLIN_PARITY_ONLY',
            'legacy_repo':str(legacy_repo),'gradle_cache':str(cache),'java':str(java),'java_sha256':sha(java),
            'legacy_commit':baseline['source_commit'],'legacy_commit_blob_sha256':git_blobs,'legacy_commit_comparison':'CRLF_NORMALIZED',
            'inputs_sha256':before,'jar_sha256':sha(compiled),
            'events_compared':len(new),'pending_restart_cases':2,'device_qualified':False,'commands':len(records)},indent=2)+'\n')
        print(out)
    except Exception as error:
        phase='COMMAND_START' if isinstance(error,CommandStartError) else 'COMMAND_TIMEOUT' if isinstance(error,CommandTimeoutError) else 'EXECUTION_OR_VALIDATION'
        (out/'failure.json').write_text(json.dumps({'state':'FAILED_OR_INCOMPLETE','phase':phase,'error':str(error)},indent=2)+'\n');raise

if __name__=='__main__':main()
