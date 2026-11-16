"""Exercise the actual host replay executable with independent expected results.

Run with --exe <built executable> --out-dir <new evidence directory>.
The directory and all fixtures/logs are retained, including failures.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / 'tests/fixtures/timeline/deadline_pressure.jsonl'
SEEK_FIXTURE = ROOT / 'tests/fixtures/timeline/seek_speed.jsonl'

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True, type=Path)
    parser.add_argument('--out-dir', required=True, type=Path)
    args = parser.parse_args()
    exe = args.exe.resolve(strict=True)
    out = args.out_dir.absolute()
    out.mkdir(parents=True, exist_ok=False)
    records = []
    def command(label, argv):
        run = subprocess.run(list(map(str, argv)), stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        (out / (label + '.stdout')).write_bytes(run.stdout)
        (out / (label + '.stderr')).write_bytes(run.stderr)
        records.append({'name':label,'argv':list(map(str,argv)),'exit_code':run.returncode,
                        'stdout_sha256':hashlib.sha256(run.stdout).hexdigest(),
                        'stderr_sha256':hashlib.sha256(run.stderr).hexdigest()})
        (out / 'commands.json').write_text(json.dumps(records,indent=2)+'\n',encoding='utf-8')
        return run
    def write_input(label, rows):
        path=out/(label+'.jsonl')
        path.write_text(''.join(json.dumps(row,separators=(',',':'))+'\n' for row in rows),encoding='ascii')
        return path
    def replay(label, path, ok):
        result=out/(label+'.result.jsonl')
        p=command(label,[exe,'--events',path,'--out',result])
        assert (p.returncode==0)==ok, (label,p.returncode,p.stderr.decode(errors='replace'))
        assert result.exists()==ok, label
        return [json.loads(line) for line in result.read_text(encoding='utf-8').splitlines()] if ok else None
    try:
        rows=[json.loads(line) for line in FIXTURE.read_text(encoding='utf-8').splitlines()]
        result=replay('deadline',FIXTURE,True)
        assert len(result)==len(rows)
        assert result[0]['scope']=='SYNTHETIC_PLANNER_REPLAY'
        assert result[1]['generation']==1 and result[2]['clock_state']==1
        assert [result[i]['accepted'] for i in (3,4)]==[1,1]
        assert result[5]['waiting_reason']==0 and result[5]['before_id']==result[5]['after_id']==1
        assert result[6]['status']==0
        assert [result[i]['status'] for i in (7,8,10,12,13,15,16)]==[-9,1,-8,-9,1,-9,1]
        assert result[9]['effect']=='publication_suppressed_retirement_pending'
        assert result[10]['in_flight']==1 and result[12]['sample_count']==0
        assert result[15]['predicted_joint_ns']==47 and result[15]['sample_count']==1
        assert result[-1]['outstanding_jobs']==0

        # M1: a speed change invalidates the old timeline, then a seek does
        # the same. New epochs must bind fresh sources and the new clock.
        seek=replay('seek_speed',SEEK_FIXTURE,True)
        assert len(seek)==26 and seek[0]['scope']=='SYNTHETIC_PLANNER_REPLAY'
        assert [seek[i]['generation'] for i in (1,10,19)]==[1,2,3]
        assert seek[7]['before_id']==1 and seek[7]['after_id']==2 and 0.49<seek[7]['alpha']<0.51
        assert seek[9]['clock_state']==4 and seek[9]['discarded']==[1,2]
        assert seek[14]['before_id']==seek[14]['after_id']==3
        assert seek[16]['media_us']==50000 and seek[16]['before_id']==3 and seek[16]['after_id']==4
        assert 0.49<seek[16]['alpha']<0.51
        assert seek[18]['clock_state']==4 and seek[18]['discarded']==[3,4]
        assert seek[23]['media_us']==1000000 and seek[23]['before_id']==seek[23]['after_id']==5
        assert seek[25]['outstanding_jobs']==0 and seek[25]['discarded']==[5,6]

        # Independent frame timeline expectations: VFR interpolation, followed
        # by a new epoch that discards old inputs and no longer has a clock.
        vfr=copy.deepcopy(rows[:7])
        vfr += [dict(seq=8,op='plan',now_ns=16666666),dict(seq=9,op='commit',now_ns=16666666),
                dict(seq=10,op='begin',media=2,decoder=1,surface=1,format=1,discontinuity=2),
                dict(seq=11,op='plan',now_ns=20000000),dict(seq=12,op='close')]
        parsed=replay('vfr_epoch',write_input('vfr_epoch_input',vfr),True)
        assert parsed[7]['waiting_reason']==0 and parsed[7]['before_id']==1 and parsed[7]['after_id']==2
        assert 0.49<parsed[7]['alpha']<0.51 and parsed[7]['mode']!=parsed[5]['mode']
        assert parsed[9]['generation']==2 and parsed[9]['discarded']==[1,2]
        assert parsed[10]['waiting_reason']==3

        # Admission is part of a live timeline epoch. Closing/restarting the
        # epoch stops new reservations but does not retire an earlier job.
        reserve=dict(op='reserve',job_id=41,now_ns=100,deadline_ns=200)
        before=[copy.deepcopy(rows[0]),dict(seq=2,**reserve),dict(seq=3,op='retire',job_id=41,joint_ns=10,succeeded=1)]
        replay('reserve_before_begin',write_input('reserve_before_begin_input',before),False)
        after=[copy.deepcopy(rows[0]),copy.deepcopy(rows[1]),dict(seq=3,op='close'),dict(seq=4,**reserve),
               dict(seq=5,op='retire',job_id=41,joint_ns=10,succeeded=1)]
        replay('reserve_after_close',write_input('reserve_after_close_input',after),False)
        restart_clock=copy.deepcopy(rows[2]);restart_clock['discontinuity']=2
        restart=[copy.deepcopy(rows[0]),copy.deepcopy(rows[1]),restart_clock,dict(seq=4,**reserve),dict(seq=5,op='close')]
        replay('reserve_after_restart',write_input('reserve_after_restart_input',restart),False)
        retired_after_close=[copy.deepcopy(rows[0]),copy.deepcopy(rows[1]),dict(seq=3,**reserve),dict(seq=4,op='close'),
                             dict(seq=5,op='cancel',job_id=41),dict(seq=6,op='retire',job_id=41,joint_ns=10,succeeded=0)]
        postclose=replay('retire_after_close',write_input('retire_after_close_input',retired_after_close),True)
        assert postclose[2]['status']==1 and postclose[3]['outstanding_jobs']==1 and postclose[5]['status']==0
        retired_after_restart=[copy.deepcopy(rows[0]),copy.deepcopy(rows[1]),dict(seq=3,**reserve),
            dict(seq=4,op='clock',media_us=0,monotonic_ns=0,speed_milli=1000,discontinuity=2,uncertainty_us=0,paused=0),
            dict(seq=5,op='retire',job_id=41,joint_ns=10,succeeded=0),dict(seq=6,op='close')]
        postrestart=replay('retire_after_restart',write_input('retire_after_restart_input',retired_after_restart),True)
        assert postrestart[3]['clock_state']==4 and postrestart[4]['status']==0

        bad=copy.deepcopy(rows[:10]);bad += [dict(seq=11,op='retire',job_id=2,joint_ns=1,succeeded=1)]
        replay('cancelled_success',write_input('cancelled_success_input',bad),False)
        replay('unretired',write_input('unretired_input',rows[:10]),False)
        bad=copy.deepcopy(rows[:12]);bad += [dict(seq=13,op='reserve',job_id=2,now_ns=100,deadline_ns=200)]
        replay('reused_job',write_input('reused_job_input',bad),False)
        bad=copy.deepcopy(rows[:3]);bad[2]['paused']=2
        replay('invalid_paused',write_input('invalid_paused_input',bad),False)
        bad=copy.deepcopy(rows[:2]);bad[1]['seq']=3
        replay('sequence_gap',write_input('sequence_gap_input',bad),False)
        bad=copy.deepcopy(rows[:1]);bad[0]['extra']=1
        replay('unknown_field',write_input('unknown_field_input',bad),False)
        bad=copy.deepcopy(rows[:1]);bad[0]['enabled']=True
        replay('boolean_not_integer',write_input('boolean_input',bad),False)
        bad=copy.deepcopy(rows[:1]);bad[0]['cold_cost_ns']=2**63
        replay('integer_overflow',write_input('overflow_input',bad),False)
        bad=out/'duplicate_input.jsonl';bad.write_text('{"seq":1,"op":"config","op":"config"}\n',encoding='ascii')
        replay('duplicate_key',bad,False)
        bad=out/'long_input.jsonl';bad.write_bytes(b' '+b' '*8192+b'\n')
        replay('long_line',bad,False)
        bad=out/'trailing_input.jsonl';bad.write_text(json.dumps(rows[0])+' true\n',encoding='ascii')
        replay('trailing_data',bad,False)
        existing=out/'existing.result.jsonl';existing.write_bytes(b'preserve existing output\n')
        old=sha(existing);p=command('existing_output',[exe,'--events',FIXTURE,'--out',existing])
        assert p.returncode!=0 and sha(existing)==old
        # Existing M1 C++/Kotlin oracle outputs, normalized only for CRLF.
        legacy_hashes={'normal':'17e96c9a87fc557990f19e28f1f960159f94ddeeb75ac9e7079c56f5a9eef3a4',
                       'delayed':'175e02c4d3e3201f01e4fe33eac3be5b2cf5e0cbe4e20828207f80f78842f5f2'}
        for name,flag in [('normal',[]),('delayed',['--delayed'])]:
            p=command('legacy_'+name,[exe,*flag])
            assert p.returncode==0 and hashlib.sha256(p.stdout.replace(b'\r\n',b'\n')).hexdigest()==legacy_hashes[name]
        p=command('invalid_args',[exe,'--wrong']);assert p.returncode!=0
        sources=[ROOT/'examples/headless_timeline.cpp',ROOT/'examples/trace_replay.hpp',Path(__file__).resolve(),FIXTURE,SEEK_FIXTURE]
        (out/'summary.json').write_text(json.dumps({'state':'PASS','scope':'SYNTHETIC_HOST_REPLAY_ONLY',
            'exe_sha256':sha(exe),'sources_sha256':{str(p):sha(p) for p in sources},
            'device_qualified':False,'actual_presentation':'NOT_EVALUATED','commands':len(records)},indent=2)+'\n',encoding='utf-8')
        print(out)
    except Exception as error:
        (out/'failure.json').write_text(json.dumps({'state':'FAILED_OR_INCOMPLETE','error':repr(error)},indent=2)+'\n',encoding='utf-8')
        raise

if __name__=='__main__':
    main()
