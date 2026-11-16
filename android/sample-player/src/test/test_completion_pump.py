"""Run the actual completion-pump methods with controlled host substitutes."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument("--javac", type=Path, required=True)
parser.add_argument("--java", type=Path, required=True)
parser.add_argument("--out", type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
main = root / "src/main/java/io/github/jichuo1/meguviz/media/player"
tests = root / "src/test/java/io/github/jichuo1/meguviz/media/player"
activity = (main / "PlayerActivity.java").read_text(encoding="utf-8")
out = args.out.resolve() / ("run-" + str(time.time_ns()))
out.mkdir(parents=True)


def method(signature):
    start = activity.index(signature)
    opening = activity.index("{", start)
    depth = 0
    for index in range(opening, len(activity)):
        if activity[index] == "{": depth += 1
        elif activity[index] == "}":
            depth -= 1
            if depth == 0: return activity[start:index + 1]
    raise ValueError("unterminated " + signature)


draw = method("@Override public void onDrawFrame")
reset = method("boolean reset()")
assert draw.index("pollCompletions()") < draw.index("session.submit(")
assert "session != null && !unsupported && !completionState.deviceLost()" in draw
assert "completionState.deviceLost()||unsupported" in draw
assert draw.index("retire()") < draw.index("pollCompletions()")
assert "checkProbe(session.probe())" in draw
assert reset.index("completionState.deviceLost()") < reset.index("session.beginEpoch(")

probe = r'''package io.github.jichuo1.meguviz.media.player;
import java.util.ArrayDeque;
import java.util.ArrayList;
final class PumpProbe {
    static final class AndroidMediaSession {
        static final class MediaException extends RuntimeException { final int status; MediaException(int s){status=s;} }
        static final class Frame { boolean closed,lose; void close(){closed=true;} }
        static final class Completed { final int status; final long jobId; final Frame frame;
            Completed(int s,long j,Frame f){status=s;jobId=j;frame=f;} }
        final ArrayDeque<Completed> queue=new ArrayDeque<>(); int status=0,draws,polls,closes,closeStatus;
        int poll(long n){++polls;return status;}
        Completed pollCompleted(){return queue.poll();}
        void draw(Frame f,int w,int h){if(f.lose)throw new MediaException(-11);++draws;}
        void close(){++closes;if(closeStatus!=0)throw new MediaException(closeStatus);}
    }
    static final class View {int posted; Runnable delayed; boolean postDelayed(Runnable r,long delay){++posted;delayed=r;return true;} void requestRender(){} }
    static final class Pending { Object value;Object peek(){return value;} }
    final CompletionState completionState=new CompletionState();
    final DeferredRetry completionRetry=new DeferredRetry(),retirementRetry=new DeferredRetry();
    final ArrayList<AndroidMediaSession> retiring=new ArrayList<>();
    final Pending pendingStart=new Pending();Object sessionContext,sessionDisplay;Thread sessionThread;boolean contextWarning;
    final View view=new View(); boolean resumed=true;int surfaceWidth=1,surfaceHeight=1,reports;
    AndroidMediaSession session=new AndroidMediaSession();AndroidMediaSession.Completed lostCompletion;
    void report(String ignored){++reports;}
__METHODS__
    static void require(boolean value,String text){if(!value)throw new AssertionError(text);}
    public static void main(String[] args){
        PumpProbe waiting=new PumpProbe(); waiting.completionState.accepted();
        require(!waiting.pollCompletions() && waiting.view.posted==1,"missing completion schedules retry");
        waiting.pollCompletions();require(waiting.view.posted==1,"only one retry queued");
        PumpProbe normal=new PumpProbe();normal.completionState.accepted();normal.completionState.accepted();
        AndroidMediaSession.Frame ok=new AndroidMediaSession.Frame();
        normal.session.queue.add(new AndroidMediaSession.Completed(0,1,ok));
        normal.session.queue.add(new AndroidMediaSession.Completed(-9,2,null));
        require(normal.pollCompletions() && ok.closed && normal.session.draws==1,"success drawn and deadline drained");
        require(!normal.completionState.needsPoll()&&!normal.completionState.deviceLost(),"normal queue remains usable");
        normal.completionState.accepted();
        PumpProbe pending=new PumpProbe();pending.pendingStart.value=new Object();pending.completionState.accepted();pending.completionState.accepted();
        AndroidMediaSession.Frame stale=new AndroidMediaSession.Frame();pending.session.queue.add(new AndroidMediaSession.Completed(0,4,stale));
        pending.session.queue.add(new AndroidMediaSession.Completed(-11,5,null));
        require(!pending.pollCompletions()&&stale.closed&&pending.session.draws==0&&pending.completionState.deviceLost(),"pending source suppresses old draw but observes loss");
        PumpProbe lost=new PumpProbe();lost.completionState.accepted();lost.completionState.accepted();
        AndroidMediaSession.Frame first=new AndroidMediaSession.Frame(), retained=new AndroidMediaSession.Frame();
        lost.session.queue.add(new AndroidMediaSession.Completed(0,1,first));
        AndroidMediaSession.Completed bad=new AndroidMediaSession.Completed(-11,2,retained);lost.session.queue.add(bad);
        require(lost.pollCompletions()&&first.closed&&lost.completionState.deviceLost(),"drains until device lost");
        require(lost.lostCompletion==bad&&!retained.closed&&lost.view.posted==0,"failed completion is retained");
        PumpProbe drawLost=new PumpProbe();drawLost.completionState.accepted();
        AndroidMediaSession.Frame failedDraw=new AndroidMediaSession.Frame();failedDraw.lose=true;
        drawLost.session.queue.add(new AndroidMediaSession.Completed(0,3,failedDraw));
        require(!drawLost.pollCompletions()&&drawLost.completionState.deviceLost()&&!failedDraw.closed,"draw device loss retains frame");
        PumpProbe pollLost=new PumpProbe();pollLost.completionState.accepted();pollLost.session.status=-11;
        require(!pollLost.pollCompletions()&&pollLost.completionState.deviceLost()&&pollLost.reports==1,"poll device loss observed");
        PumpProbe retirePoll=new PumpProbe();AndroidMediaSession earlier=new AndroidMediaSession(),failed=new AndroidMediaSession();failed.status=-11;
        retirePoll.retiring.add(earlier);retirePoll.retiring.add(failed);retirePoll.retire();
        require(retirePoll.completionState.deviceLost()&&failed.closes==0&&earlier.polls==0&&retirePoll.retiring.size()==2,"retire poll loss retains all and stops");
        PumpProbe retireClose=new PumpProbe();AndroidMediaSession closeLost=new AndroidMediaSession();closeLost.closeStatus=-11;
        retireClose.retiring.add(closeLost);retireClose.retire();
        require(retireClose.completionState.deviceLost()&&closeLost.closes==1&&retireClose.retiring.size()==1,"retire close loss retains session");
        PumpProbe busy=new PumpProbe();AndroidMediaSession busySession=new AndroidMediaSession();busySession.status=-12;busy.retiring.add(busySession);busy.retire();
        require(!busy.completionState.deviceLost()&&busySession.closes==0&&busy.retiring.size()==1&&busy.view.posted==1,"busy retire waits without close");
        PumpProbe probeLoss=new PumpProbe();require(!probeLoss.checkProbe(new long[]{-11})&&probeLoss.completionState.deviceLost(),"probe device loss is sticky");
        require(new PumpProbe().checkProbe(new long[]{0}),"probe success remains usable");
        System.out.println("PASS extracted production completion methods; host substitutes only");
    }
}
'''.replace("__METHODS__", "\n".join(method(s) for s in
    ("private void deviceLost()", "private void retryCompletions()", "private boolean checkProbe(", "private boolean pollCompletions()", "private void retire()")))
path = out / "PumpProbe.java"
path.write_text(probe, encoding="utf-8")
classes = out / "classes"
classes.mkdir()
records = []


def run(name, command):
    result = subprocess.run([str(x) for x in command], capture_output=True, timeout=60)
    (out / (name + ".stdout")).write_bytes(result.stdout)
    (out / (name + ".stderr")).write_bytes(result.stderr)
    records.append({"name": name, "argv": [str(x) for x in command], "exit_code": result.returncode,
                    "stdout_sha256": hashlib.sha256(result.stdout).hexdigest(), "stderr_sha256": hashlib.sha256(result.stderr).hexdigest()})
    (out / "commands.json").write_text(json.dumps(records, indent=2), encoding="utf-8")
    if result.returncode: raise RuntimeError(name + " failed")


run("compile", [args.javac, "-d", classes, main / "CompletionState.java", main / "DeferredRetry.java", tests / "CompletionStateTest.java", path])
for name in ("CompletionStateTest", "PumpProbe"):
    run(name, [args.java, "-cp", classes, "io.github.jichuo1.meguviz.media.player." + name])
inputs = [main / "PlayerActivity.java", main / "CompletionState.java", main / "DeferredRetry.java", tests / "CompletionStateTest.java", Path(__file__).resolve()]
(out / "summary.json").write_text(json.dumps({"state": "PASS", "scope": "actual extracted Java completion methods with host substitutes; static draw order",
    "inputs": {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs},
    "device_qualification": False, "gpu_retirement": "NOT_TESTED"}, indent=2), encoding="utf-8")
print(out)
