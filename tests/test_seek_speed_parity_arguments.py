"""Check that invalid parity-tool inputs fail before creating their output."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]

def main():
    p=argparse.ArgumentParser();p.add_argument('--exe',type=Path,required=True);p.add_argument('--out-dir',type=Path,required=True)
    p.add_argument('--legacy-repo',type=Path,default=Path(r'D:\Bilibili_Innocent_Lab'))
    p.add_argument('--gradle-cache',type=Path,default=Path(r'D:\Gradle\caches\modules-2\files-2.1'))
    p.add_argument('--java',type=Path,default=Path(r'C:\Program Files\Android\Android Studio\jbr\bin\java.exe'))
    a=p.parse_args();out=a.out_dir.absolute();out.mkdir(parents=True,exist_ok=False)
    empty_repo=out/'empty-repo';empty_repo.mkdir();empty_cache=out/'empty-cache';empty_cache.mkdir()
    records=[];tool=ROOT/'tests/test_seek_speed_parity.py'
    cases=[('missing-repo','--legacy-repo',out/'missing-repo'),('wrong-repo','--legacy-repo',empty_repo),
           ('missing-cache','--gradle-cache',out/'missing-cache'),('empty-cache','--gradle-cache',empty_cache),
           ('missing-java','--java',out/'missing-java'),('java-is-directory','--java',out)]
    for name,option,value in cases:
        target=out/(name+'-output')
        values={'--legacy-repo':a.legacy_repo,'--gradle-cache':a.gradle_cache,'--java':a.java};values[option]=value
        argv=[str(Path(sys.executable)), '-B',str(tool),'--exe',str(a.exe),'--out-dir',str(target)]
        for key,path in values.items():argv.extend([key,str(path)])
        result=subprocess.run(argv,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=30)
        (out/(name+'.stdout')).write_bytes(result.stdout);(out/(name+'.stderr')).write_bytes(result.stderr)
        if result.returncode==0 or target.exists():raise RuntimeError(name+' did not fail closed')
        records.append({'name':name,'argv':argv,'exit_code':result.returncode,'output_created':False,
            'stdout_sha256':hashlib.sha256(result.stdout).hexdigest(),'stderr_sha256':hashlib.sha256(result.stderr).hexdigest()})
    # This is a known text fixture, not an unknown executable or a version probe.
    nonexe=out/'not-an-executable.txt';nonexe.write_text('not an executable\n',encoding='ascii')
    target=out/'nonexecutable-java-output'
    argv=[str(Path(sys.executable)),'-B',str(tool),'--exe',str(a.exe),'--out-dir',str(target),
          '--legacy-repo',str(a.legacy_repo),'--gradle-cache',str(a.gradle_cache),'--java',str(nonexe)]
    result=subprocess.run(argv,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=30)
    (out/'nonexecutable-java.stdout').write_bytes(result.stdout);(out/'nonexecutable-java.stderr').write_bytes(result.stderr)
    if result.returncode==0:raise RuntimeError('nonexecutable Java unexpectedly succeeded')
    failure=json.loads((target/'failure.json').read_text());attempts=json.loads((target/'commands.json').read_text())
    if failure.get('phase')!='COMMAND_START' or len(attempts)!=3:raise RuntimeError('missing command start failure evidence')
    if [entry['name'] for entry in attempts[:2]]!=['git-show-FrameTimeline','git-show-FrameRateTarget']:
        raise RuntimeError('missing git show command evidence')
    attempt=attempts[2]
    if attempt.get('name')!='compile' or attempt.get('state')!='START_FAILED' or attempt.get('process_started') is not False or Path(attempt['argv'][0])!=nonexe:
        raise RuntimeError('missing failed spawn argv')
    if not (target/'compile.stderr').is_file():raise RuntimeError('missing spawn stderr')
    records.append({'name':'nonexecutable-java','argv':argv,'exit_code':result.returncode,'output_created':True,
        'stdout_sha256':hashlib.sha256(result.stdout).hexdigest(),'stderr_sha256':hashlib.sha256(result.stderr).hexdigest(),
        'failure_phase':failure['phase'],'command_state':attempt['state']})
    (out/'summary.json').write_text(json.dumps({'state':'PASS','scope':'INVALID_PATHS_FAIL_CLOSED',
        'parity_tool_sha256':hashlib.sha256(tool.read_bytes()).hexdigest(),
        'test_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'exe_sha256':hashlib.sha256(a.exe.read_bytes()).hexdigest(),'cases':records},indent=2)+'\n')
    print(out)

if __name__=='__main__':main()
