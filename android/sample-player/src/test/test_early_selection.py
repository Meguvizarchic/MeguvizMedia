"""Exercise the actual selection callback while the old decoder is blocked."""
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
source = root / "src/main/java/io/github/jichuo1/meguviz/media/player"
files = [source / "PlayerActivity.java", source / "DecoderGate.java", source / "DeferredStart.java", Path(__file__).resolve()]
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
before = {str(p): sha(p) for p in files}
text = files[0].read_text(encoding="utf-8")
start = text.index("@Override protected void onActivityResult")
opening = text.index("{", start)
depth = 0
for end in range(opening, len(text)):
    if text[end] == "{": depth += 1
    elif text[end] == "}":
        depth -= 1
        if not depth: break
else: raise RuntimeError("unterminated onActivityResult")
actual = text[start:end + 1]
out = args.out.resolve() / ("run-" + str(time.time_ns()))
out.mkdir(parents=True)
probe = r'''package io.github.jichuo1.meguviz.media.player;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
class Activity {
  static final int RESULT_OK=-1;
  protected void onActivityResult(int request,int result,Intent data) {}
}
class Uri { final String value; Uri(String value){this.value=value;} }
class Intent { final Uri uri; Intent(Uri uri){this.uri=uri;} Uri getData(){return uri;} }
class EarlySelectionProbe extends Activity {
  static class View { int requests; void requestRender(){++requests;} }
  final View view=new View(); final DecoderGate decoder=new DecoderGate();
  final DeferredStart<Uri> pendingStart=new DeferredStart<>();
  Uri selectedSource; boolean resumeSelected=true;
  final CountDownLatch forbiddenWait=new CountDownLatch(1); volatile boolean stopCalled;
  private long stopDecoder(){stopCalled=true;try{forbiddenWait.await();}catch(InterruptedException e){Thread.currentThread().interrupt();}return decoder.cancel();}
__CALLBACK__
  static void require(boolean ok,String why){if(!ok)throw new AssertionError(why);}
  public static void main(String[] args)throws Exception {
    EarlySelectionProbe p=new EarlySelectionProbe();
    CountDownLatch entered=new CountDownLatch(1),release=new CountDownLatch(1),returned=new CountDownLatch(1);
    AtomicBoolean sawCancel=new AtomicBoolean();long initial=p.decoder.cancel();
    require(p.decoder.start(initial,new Object(),token->{entered.countDown();try{release.await();}catch(InterruptedException e){Thread.currentThread().interrupt();}sawCancel.set(token.get());return true;}),"old decoder start");
    require(entered.await(5,TimeUnit.SECONDS),"old decoder entered");
    Thread ui=new Thread(()->{p.onActivityResult(1,RESULT_OK,new Intent(new Uri("first")));returned.countDown();});ui.start();
    try {
      require(returned.await(3,TimeUnit.SECONDS),"selection waited for old decoder");
      require(!p.stopCalled&&p.view.requests==1,"selection must publish without join");
      DeferredStart.Request<Uri> first=p.pendingStart.peek();
      require(first!=null&&first.source.value.equals("first")&&p.decoder.current(first.generation),"request and generation visible");
      require(p.selectedSource==first.source&&!p.resumeSelected,"selection replaces the saved resume source");
      require(p.pendingStart.takeIfReady(true,p.decoder)==null&&p.pendingStart.peek()==first,"old decoder still prevents new producer");
      p.onActivityResult(1,RESULT_OK,new Intent(new Uri("second")));
      DeferredStart.Request<Uri> second=p.pendingStart.peek();
      require(second!=null&&second.source.value.equals("second")&&!p.decoder.current(first.generation),"new selection invalidates old generation");
      p.onActivityResult(1,0,new Intent(new Uri("ignored")));
      require(p.pendingStart.peek()==second,"cancelled picker does not change pending request");
      release.countDown();require(p.decoder.awaitStopped(5000)&&sawCancel.get(),"old decoder actually stopped after cancel");
      require(p.pendingStart.takeIfReady(true,p.decoder)==second,"latest source starts only after decoder exits");
      long stale=p.decoder.cancel();p.pendingStart.offer(new Uri("stale"),stale);p.decoder.cancel();
      require(p.pendingStart.takeIfReady(true,p.decoder)==null&&p.pendingStart.peek()==null,"cancelled generation rejected");
      System.out.println("PASS actual selection callback publishes before old decoder exits");
    } finally {release.countDown();p.forbiddenWait.countDown();ui.join(5000);}
  }
}
'''.replace("__CALLBACK__", actual)
probe_path = out / "EarlySelectionProbe.java"
probe_path.write_text(probe, encoding="utf-8")
classes = out / "classes"
classes.mkdir()
commands = []


def run(name, argv):
    argv = [str(x) for x in argv]
    process = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
    (out / (name + ".stdout")).write_bytes(process.stdout)
    (out / (name + ".stderr")).write_bytes(process.stderr)
    commands.append({"name": name, "argv": argv, "exit_code": process.returncode,
                     "stdout_sha256": sha(out / (name + ".stdout")), "stderr_sha256": sha(out / (name + ".stderr"))})
    (out / "commands.json").write_text(json.dumps(commands, indent=2), encoding="utf-8")
    if process.returncode: raise RuntimeError(name + " failed")


run("compile", [args.javac, "-d", classes, source / "DecoderGate.java", source / "DeferredStart.java", probe_path])
run("test", [args.java, "-cp", classes, "io.github.jichuo1.meguviz.media.player.EarlySelectionProbe"])
if before != {str(p): sha(p) for p in files}: raise RuntimeError("recorded inputs changed")
(out / "summary.json").write_text(json.dumps({"state": "PASS", "scope": "actual selection callback and decoder gate with host Android substitutes",
    "source_sha256": before, "source_unchanged_during_run": True, "android_lifecycle": "NOT_TESTED", "device": "NOT_TESTED"}, indent=2), encoding="utf-8")
print(out)
