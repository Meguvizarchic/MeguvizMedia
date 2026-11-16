"""Check binary identity and exclusive native ownership; no device claims."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

parser=argparse.ArgumentParser()
parser.add_argument("--manifest",type=Path,required=True)
parser.add_argument("--jni-aar",type=Path)
parser.add_argument("--apk",type=Path)
parser.add_argument("--out",type=Path,required=True)
args=parser.parse_args()
manifest=json.loads(args.manifest.read_text(encoding="utf-8"))
root=args.manifest.resolve().parent
def sha(data):return hashlib.sha256(data).hexdigest()
def checked(entry):
    path=(root/entry["path"]).resolve()
    if not path.is_relative_to(root):raise ValueError("artifact path escapes candidate")
    data=path.read_bytes()
    if sha(data)!=entry["sha256"]:raise ValueError("artifact SHA mismatch: "+str(path))
    if "size_bytes" in entry and len(data)!=entry["size_bytes"]:raise ValueError("artifact size mismatch")
    return data
lock=json.loads(checked(manifest["composition_lock"]))
openmp=lock.get("toolchain",{}).get("android_static_openmp","OFF")
if openmp not in ("ON","OFF"):raise ValueError("Invalid OpenMP profile")
shared_plan=lock.get("toolchain",{}).get("shared_plan")
if shared_plan is not None and (shared_plan not in ("ON","OFF") or
    any(entry.get("shared_plan")!=shared_plan for entry in manifest["abis"])):
    raise ValueError("Shared plan profile differs from composition lock")
notices=[item for item in manifest.get("notices",[]) if item.get("path")=="notices/llvm-openmp-NOTICE.txt"]
if (openmp=="ON" and len(notices)!=1) or (openmp=="OFF" and notices):raise ValueError("OpenMP notice/profile mismatch")
if openmp=="ON" and lock.get("toolchain",{}).get("openmp_notice")!=notices[0]:raise ValueError("OpenMP notice differs from composition lock")
openmp_notice=checked(notices[0]) if notices else None
api=checked(manifest["jvm_api"])
with zipfile.ZipFile(root/manifest["jvm_api"]["path"]) as z:
    if any(name.endswith(".so") or name.startswith("jni/") for name in z.namelist()):raise ValueError("API AAR owns native binaries")
expected={e["abi"]:checked(e) for e in manifest["abis"]}
results=[]
for path,prefix in ((args.jni_aar,"jni"),(args.apk,"lib")):
    if path is None:continue
    with zipfile.ZipFile(path) as z:
        notice_name="assets/licenses/llvm-openmp-NOTICE.txt"
        if openmp_notice is not None:
            if z.namelist().count(notice_name)!=1 or z.read(notice_name)!=openmp_notice:
                raise ValueError("APK/AAR lacks the exact locked OpenMP notice")
        elif notice_name in z.namelist():raise ValueError("OFF package unexpectedly contains OpenMP notice")
        native=[name for name in z.namelist() if name.endswith(".so")]
        wanted={prefix+"/"+abi+"/libmeguviz_media_rife_ncnn.so" for abi in expected}
        if set(native)!=wanted:raise ValueError("Expected exactly one canonical DSO per ABI: "+str(native))
        if any(name.startswith("assets/") and ("native-payload" in name or name.endswith("VisionTool.zip")) for name in z.namelist()):raise ValueError("JNI owner and payload owner coexist")
        for abi,data in expected.items():
            name=prefix+"/"+abi+"/libmeguviz_media_rife_ncnn.so"
            if z.read(name)!=data:raise ValueError("AGP changed canonical DSO bytes: "+name)
        results.append(dict(path=str(path.resolve()),size_bytes=path.stat().st_size,sha256=sha(path.read_bytes()),native_owner="Media",canonical_bytes_equal=True))
args.out.parent.mkdir(parents=True,exist_ok=True)
args.out.write_text(json.dumps(dict(schema_version=1,result="PASS",scope="PACKAGING_IDENTITY_ONLY",device_executed=False,artifacts=results),indent=2)+"\n",encoding="utf-8")
print("canonical native identity PASS",flush=True)
