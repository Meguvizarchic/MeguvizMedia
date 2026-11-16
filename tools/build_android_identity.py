"""Build the current pure-core identity JNI profile, including timing facades.
Historic M1 receipts are not overwritten. No RIFE/ncnn production dependency.
"""
import datetime
import hashlib
import json
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[1];SDK=Path(r"C:\Users\Administrator\AppData\Local\Android\Sdk")
NDK=SDK/"ndk/30.0.16248370";CMAKE=SDK/"cmake/4.1.2/bin/cmake.exe";NINJA=CMAKE.with_name("ninja.exe")
OUT=ROOT/"artifacts/M6/identity-sdk";OUT.mkdir(parents=True,exist_ok=True)
def run(label,argv):
    argv=list(map(str,argv));stamp=datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ");key=label+"-"+stamp
    p=subprocess.run(argv,cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    (OUT/(key+".stdout.txt")).write_bytes(p.stdout);(OUT/(key+".stderr.txt")).write_bytes(p.stderr)
    with (OUT/"commands.jsonl").open("a",encoding="utf-8") as stream:stream.write(json.dumps(dict(cwd=str(ROOT),argv=argv,exit_code=p.returncode,started_utc=stamp,stdout=key+".stdout.txt",stderr=key+".stderr.txt",device_executed=False))+"\n")
    print(label,p.returncode,flush=True)
    if p.returncode:raise RuntimeError(label+" failed; raw output retained")
files=[]
for abi in ("arm64-v8a","armeabi-v7a"):
    build=ROOT/"build"/("identity-sdk-"+abi);prefix=OUT/("android-"+abi)/"install";jni=ROOT/"build"/("identity-jni-"+abi)
    common=["-G","Ninja","-DCMAKE_MAKE_PROGRAM="+str(NINJA),"-DCMAKE_TOOLCHAIN_FILE="+str(NDK/"build/cmake/android.toolchain.cmake"),"-DANDROID_ABI="+abi,"-DANDROID_PLATFORM=android-27","-DANDROID_STL=c++_static","-DCMAKE_BUILD_TYPE=Release"]
    run(abi+"-core-configure",[CMAKE,"-S",ROOT,"-B",build,*common,"-DMGM_BUILD_TESTS=OFF","-DMGM_WITH_RIFE=OFF","-DMGC_WITH_NCNN=OFF","-DMGC_VULKAN=OFF","-DMGM_COMPUTE_SOURCE="+str(ROOT.parent/"MeguvizCompute")])
    run(abi+"-core-build",[CMAKE,"--build",build,"-j","2"])
    run(abi+"-core-install",[CMAKE,"--install",build,"--prefix",prefix])
    run(abi+"-jni-configure",[CMAKE,"-S",ROOT/"distribution/android","-B",jni,*common,"-DMGM_ANDROID_COMPOSITION=OFF","-DMGM_IDENTITY_SDK_PREFIX="+str(prefix)])
    run(abi+"-jni-build",[CMAKE,"--build",jni,"-j","2"])
    library=jni/"libmeguviz_media_identity.so"
    files.append(dict(abi=abi,path=str(library.relative_to(ROOT)),size_bytes=library.stat().st_size,sha256=hashlib.sha256(library.read_bytes()).hexdigest()))
(OUT/"manifest.json").write_text(json.dumps(dict(schema_version=1,scope="PURE_CORE_IDENTITY_AND_TIMING_FACADES_BUILD_ONLY",device_executed=False,rife=False,ncnn=False,abis=files),indent=2)+"\n",encoding="utf-8")
