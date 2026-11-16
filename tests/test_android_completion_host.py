"""Run production JNI completion/queue sections with controlled host stubs.

Run in a developer shell with MSVC cl.exe; output is evidence, not device proof.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument("--out", required=True, type=Path)
parser.add_argument("--source", type=Path)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[1]
source = args.source.resolve() if args.source else repo / "distribution/android/android_jni.cpp"
template = Path(__file__).with_name("android_completion_host.cpp.in")
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
thread_header = repo / "src/android/thread_identity.hpp"
source_before, template_before, thread_before = sha(source), sha(template), sha(thread_header)
text = source.read_text(encoding="utf-8")
sections = {
    "@SESSION_AND_REGISTRY@": text[text.index("enum AsyncStage {"):text.index("jlongArray values(")],
    "@OBSERVATION_AND_COMPLETION@": text[text.index("MgmEvent observation("):text.index("\n}\n#define JNI(name)")],
    "@RETAIN_UNPUBLISHED_FRAME@": text[text.index("static void retain_unpublished_frame("):text.index("struct RawFrameBatch {")],
    "@POLL_COMPLETED@": text[text.index('extern "C" JNIEXPORT jlongArray JNICALL JNI(androidPollCompleted)'):text.index('\nextern "C" JNIEXPORT jint JNICALL JNI(androidCancel)')],
}
test = '#include "' + str(repo / "src/android/thread_identity.hpp").replace('\\', '/') + '"\n' + template.read_text(encoding="utf-8")
for marker, body in sections.items():
    if test.count(marker) != 1:
        raise RuntimeError("template marker missing or ambiguous: " + marker)
    test = test.replace(marker, body)
root = args.out.resolve() / ("run-" + str(time.time_ns()))
root.mkdir(parents=True)
unit = root / "completion_host.cpp"
unit.write_text(test, encoding="utf-8")
exe = root / "completion_host.exe"
argv = ["cl.exe", "/nologo", "/std:c++17", "/EHsc", "/utf-8", "/I" + str(repo / "include"),
        "/I" + str(repo.parent / "MeguvizCompute/include"), "/I" + str(repo.parent / "MeguvizRIFE/include"),
        "/Fe:" + str(exe), str(unit)]
commands = []
for cmd in (argv, [str(exe)]):
    proc = subprocess.run(cmd, cwd=root, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60)
    commands.append({"argv": cmd, "exit_code": proc.returncode, "stdout": proc.stdout, "stderr": proc.stderr})
    (root / "commands.json").write_text(json.dumps(commands, ensure_ascii=False, indent=2), encoding="utf-8")
    if proc.returncode:
        print(proc.stdout, proc.stderr)
        raise SystemExit(proc.returncode)
if source_before != sha(source) or template_before != sha(template) or thread_before != sha(thread_header):
    raise RuntimeError("test input changed during execution")
(root / "summary.json").write_text(json.dumps({"result": "PASS", "scope": "real extracted JNI callback and poll, controlled host stubs only",
    "source_sha256": source_before, "template_sha256": template_before, "thread_identity_sha256": thread_before, "runner_sha256": sha(Path(__file__)),
    "generated_sha256": sha(unit), "executable_sha256": sha(exe),
    "android_device_run": False, "physical_gpu_retirement": "NOT_TESTED"}, indent=2), encoding="utf-8")
print(root)
