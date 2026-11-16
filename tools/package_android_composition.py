"""Package/check the canonical payload; no installation or device execution."""
import datetime
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("--composition-dir",type=Path,default=ROOT/"artifacts/M6/android-composition")
args=parser.parse_args()
OUT=args.composition_dir.resolve(strict=True)
if not OUT.is_relative_to(ROOT.parent.resolve()):
    raise ValueError("Composition directory must be inside the task workspace")
manifest_path=OUT/"manifest.json";manifest=json.loads(manifest_path.read_text(encoding="utf-8"))
if not manifest.get("finalized"):raise RuntimeError("canonical dual-ABI payload is not finalized")
env=os.environ.copy();env["JAVA_HOME"]=r"C:\Program Files\Android\Android Studio\jbr";env["ANDROID_HOME"]=r"C:\Users\Administrator\AppData\Local\Android\Sdk"
argv=["cmd.exe","/d","/c",str(ROOT/"android/gradlew.bat"),":media-rife-jni:assembleRelease",":sample-player:assembleDebug","--offline","--console=plain","-Pmeguviz.compositionDir="+str(OUT)]
key="gradle-package-"+datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
start=datetime.datetime.now(datetime.timezone.utc).isoformat();p=subprocess.run(argv,cwd=ROOT/"android",env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
(OUT/(key+".stdout.txt")).write_bytes(p.stdout);(OUT/(key+".stderr.txt")).write_bytes(p.stderr)
with (OUT/"commands.jsonl").open("a",encoding="utf-8") as stream:stream.write(json.dumps(dict(name="gradle-package",cwd=str(ROOT/"android"),argv=argv,start_utc=start,end_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),exit_code=p.returncode,stdout=key+".stdout.txt",stderr=key+".stderr.txt",device_executed=False))+"\n")
if p.returncode:raise RuntimeError("Gradle package failed; raw evidence retained")
def descriptor(path):return dict(path=str(path.relative_to(OUT)).replace("\\","/"),size_bytes=path.stat().st_size,sha256=hashlib.sha256(path.read_bytes()).hexdigest())
copies={"jni_aar":(ROOT/"android/media-rife-jni/build/outputs/aar/media-rife-jni-release.aar","meguviz-media-rife-ncnn-jni-release.aar"),"sample_apk":(ROOT/"android/sample-player/build/outputs/apk/debug/sample-player-debug.apk","meguviz-media-player-debug.apk")}
for name,(source,target) in copies.items():
    path=OUT/target;shutil.copyfile(source,path);manifest[name]=descriptor(path)
openmp_notices=[item for item in manifest.get("notices",[]) if item.get("path")=="notices/llvm-openmp-NOTICE.txt"]
if len(openmp_notices)>1:raise RuntimeError("Duplicate OpenMP notice")
lock_record=manifest.get("composition_lock",{})
lock_path=OUT/lock_record.get("path","")
if not lock_path.is_file() or hashlib.sha256(lock_path.read_bytes()).hexdigest()!=lock_record.get("sha256"):
    raise RuntimeError("Composition lock identity mismatch")
lock=json.loads(lock_path.read_text(encoding="utf-8"))
if lock.get("toolchain",{}).get("android_static_openmp")=="ON" and len(openmp_notices)!=1:
    raise RuntimeError("OpenMP-enabled composition requires its LLVM notice")
if openmp_notices:
    item=openmp_notices[0];notice=OUT/item["path"]
    if descriptor(notice)!=item:raise RuntimeError("OpenMP notice identity changed")
    # The exact locked notice must be part of the Gradle-built AAR and APK;
    # adding it to an AAR after the APK build cannot establish APK coverage.
    for field in ("jni_aar","sample_apk"):
        with zipfile.ZipFile(OUT/manifest[field]["path"]) as z:
            name="assets/licenses/llvm-openmp-NOTICE.txt"
            if z.namelist().count(name)!=1 or z.read(name)!=notice.read_bytes():
                raise RuntimeError(field+" does not contain the locked OpenMP notice")
elif lock.get("toolchain",{}).get("android_static_openmp") == "OFF":
    for field in ("jni_aar","sample_apk"):
        with zipfile.ZipFile(OUT/manifest[field]["path"]) as z:
            if "assets/licenses/llvm-openmp-NOTICE.txt" in z.namelist():
                raise RuntimeError(field+" unexpectedly contains the OpenMP notice for the OFF profile")
result=subprocess.run([sys.executable,str(ROOT/"tools/inspect_android_composition.py"),"--manifest",str(manifest_path),"--jni-aar",str(OUT/manifest["jni_aar"]["path"]),"--apk",str(OUT/manifest["sample_apk"]["path"]),"--out",str(OUT/"packaging-identity.json")],cwd=ROOT)
if result.returncode:raise RuntimeError("canonical AAR/APK identity check failed")
notices=list(manifest.get("notices", []))
for project,name in (("MeguvizMedia","LICENSE"),("MeguvizMedia","NOTICE.md"),("MeguvizCompute","LICENSE"),("MeguvizCompute","NOTICE.md"),("MeguvizCompute","third_party/SBOM.json"),("MeguvizRIFE","LICENSE"),("MeguvizRIFE","src/backends/ncnn/legacy/NOTICE.md"),("MeguvizRIFE","src/backends/ncnn/legacy/upstream/LICENSE")):
    source=ROOT.parent/project/name;destination=OUT/"notices"/project/name;destination.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,destination);notices.append(descriptor(destination))
for source,name in ((ROOT.parent/"dependencies/ncnn-meguviz/LICENSE.txt","ncnn-LICENSE.txt"),(ROOT.parent/"dependencies/ncnn-meguviz/glslang/LICENSE.txt","glslang-LICENSE.txt")):
    destination=OUT/"notices"/name;shutil.copyfile(source,destination);notices.append(descriptor(destination))
manifest["notices"]=notices
archive=OUT/"native-payload.zip"
with zipfile.ZipFile(archive,"w",compression=zipfile.ZIP_DEFLATED,compresslevel=9) as z:
    for item in manifest["abis"]:z.write(OUT/item["path"],arcname=item["path"].removeprefix("native-payload/"))
    if openmp_notices:z.write(OUT/openmp_notices[0]["path"],arcname="META-INF/llvm-openmp-NOTICE.txt")
manifest["native_payload_archive"]=descriptor(archive)
manifest["packaging_identity"]=descriptor(OUT/"packaging-identity.json")
manifest_path.write_text(json.dumps(manifest,indent=2)+"\n",encoding="utf-8")
print("packaged canonical candidate",flush=True)
