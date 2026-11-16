"""Build the single static dependency closure and finalize its binary once.

This creates a build candidate, never a GPU/device/performance qualification.
The exact canonical bytes feed either jni-AAR or native-payload, exclusively.
"""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PACK = ROOT.parent
SDK = Path(r"C:\Users\Administrator\AppData\Local\Android\Sdk")
PARSER = argparse.ArgumentParser()
PARSER.add_argument("--ndk", type=Path, default=SDK / "ndk/30.0.16248370")
PARSER.add_argument("--cmake", type=Path, default=SDK / "cmake/4.1.2/bin/cmake.exe")
PARSER.add_argument("--ninja", type=Path, default=SDK / "cmake/4.1.2/bin/ninja.exe")
PARSER.add_argument("--abi", choices=["all", "arm64-v8a", "armeabi-v7a"], default="all")
PARSER.add_argument("--objects-only", action="store_true")
PARSER.add_argument("--jvm-api", type=Path)
PARSER.add_argument("--output-dir", type=Path, help="separate composition output directory")
PARSER.add_argument("--build-root", type=Path, help="separate directory for per-ABI CMake builds")
PARSER.add_argument("--openmp", choices=["on", "off"], default="on", help="Android composition runtime profile; off retains the fallback")
PARSER.add_argument("--job-observation", choices=["on", "off"], default="off", help="Optional result-owned execution observations")
PARSER.add_argument("--shared-plan", choices=["on", "off"], default="on", help="Compile the optional shared GPU plan protocol")
ARGS = PARSER.parse_args()
OUT = (ARGS.output_dir or ROOT / "artifacts/M6/android-composition").resolve()
BUILD_ROOT = (ARGS.build_root or ROOT / "build").resolve()
OUT.mkdir(parents=True, exist_ok=True)

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def run(name, argv):
    argv = list(map(str, argv))
    start = datetime.datetime.now(datetime.timezone.utc).isoformat()
    log_key = name + "-" + datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    result = subprocess.run(argv, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    (OUT / (log_key + ".stdout.txt")).write_bytes(result.stdout)
    (OUT / (log_key + ".stderr.txt")).write_bytes(result.stderr)
    with (OUT / "commands.jsonl").open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(dict(name=name, cwd=str(ROOT), argv=argv, start_utc=start,
            end_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(), exit_code=result.returncode,
            stdout=log_key + ".stdout.txt", stderr=log_key + ".stderr.txt", device_executed=False)) + "\n")
    print(name, result.returncode, flush=True)
    if result.returncode:
        raise RuntimeError("Build failed: " + name + "; retained raw logs")
    return result.stdout

def source_identity(project):
    path = PACK / project
    command = ["git", "-c", "safe.directory=" + str(path), "-C", str(path)]
    revision = subprocess.check_output(command + ["rev-parse", "HEAD"], text=True).strip()
    # Includes new implementation files; ignored build/artifact/cache files do
    # not recursively contaminate the reproducible source identity.
    names = subprocess.check_output(command + ["ls-files", "--cached", "--others", "--exclude-standard"], text=True).splitlines()
    entries = []
    for name in names:
        p = path / name
        if name in ("STATUS.json","ARTIFACTS.json","SOURCE_MANIFEST.json") or name.startswith(("artifacts/", "build/", "android/.gradle/")) or not p.is_file():
            continue
        entries.append(dict(path=name, sha256=sha(p)))
    encoded = json.dumps(entries, sort_keys=True, separators=(",", ":")).encode()
    return dict(revision=revision, source_tree_sha256=hashlib.sha256(encoded).hexdigest(), files=entries)

abis = ("arm64-v8a", "armeabi-v7a") if ARGS.abi == "all" else (ARGS.abi,)
build_infos = {}
strip = ARGS.ndk / "toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe"
readelf = strip.with_name("llvm-readelf.exe")
for abi in abis:
    build = BUILD_ROOT / ("android-composition-" + abi)
    argv = [ARGS.cmake, "-S", ROOT / "distribution/android", "-B", build, "-G", "Ninja",
        "-DCMAKE_MAKE_PROGRAM=" + str(ARGS.ninja),
        "-DCMAKE_TOOLCHAIN_FILE=" + str(ARGS.ndk / "build/cmake/android.toolchain.cmake"),
        "-DANDROID_ABI=" + abi, "-DANDROID_PLATFORM=android-27", "-DANDROID_STL=c++_static",
        "-DCMAKE_BUILD_TYPE=Release", "-DMGM_ANDROID_COMPOSITION=ON", "-DMGC_WITH_NCNN=ON",
        "-DMGM_ANDROID_STATIC_OPENMP=" + ARGS.openmp.upper(),
        "-DMGM_ANDROID_SHARED_PLAN=" + ARGS.shared_plan.upper(),
        "-DMGR_ENABLE_JOB_OBSERVATION=" + ARGS.job_observation.upper(),
        "-DMGC_VULKAN=ON",
        "-DMGC_NCNN_SOURCE=" + str(PACK / "dependencies/ncnn-meguviz"),
        "-DMGC_GLSLANG_SOURCE=" + str(PACK / "dependencies/glslang")]
    run(abi + "-configure", argv)
    if ARGS.objects_only:
        targets = ["media/CMakeFiles/meguviz_media_android.dir/src/android/ahb_bridge.cpp.o",
            "CMakeFiles/meguviz_media_rife_ncnn.dir/frame_jni.cpp.o",
            "CMakeFiles/meguviz_media_rife_ncnn.dir/android_jni.cpp.o"]
        run(abi + "-objects", [ARGS.cmake, "--build", build, "--target", *targets, "-j", "3"])
        continue
    # Build every exported/installable archive, including glslang's real
    # compatibility archives. DSO-only builds leave a broken development SDK.
    run(abi + "-build", [ARGS.cmake, "--build", build, "-j", "4"])
    payload = OUT / "native-payload" / abi / "libmeguviz_media_rife_ncnn.so"
    payload.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(build / "libmeguviz_media_rife_ncnn.so", payload)
    run(abi + "-canonical-strip", [strip, "--strip-unneeded", payload])
    run(abi + "-elf", [readelf, "-lWd", payload])
    build_info = json.loads((build / "compute/build-info.json").read_text())
    rife_info = json.loads((build / "rife/build-info.json").read_text())
    if build_info.get("android_static_openmp") != ARGS.openmp.upper():
        raise RuntimeError("Actual OpenMP profile differs from the requested composition")
    if rife_info.get("job_observation") != ARGS.job_observation.upper():
        raise RuntimeError("Actual RIFE observation profile differs from the requested composition")
    if rife_info.get("shared_plan") != ARGS.shared_plan.upper():
        raise RuntimeError("Actual RIFE shared plan profile differs from the requested composition")
    build_infos[abi] = build_info
    receipt = dict(abi=abi, path=str(payload.relative_to(OUT)).replace("\\", "/"),
        sha256=sha(payload), size_bytes=payload.stat().st_size,
        private_sdk_fingerprint=build_info["private_sdk_fingerprint"],
        rife_build_fingerprint=rife_info["build_fingerprint"], job_observation=rife_info["job_observation"],
        shared_plan=rife_info["shared_plan"],
        android_static_openmp=build_info["android_static_openmp"],
        android_openmp_runtime_sha256=build_info["android_openmp_runtime_sha256"],
        strip_tool_sha256=sha(strip), strip_options=["--strip-unneeded"],
        profile="AHB_FUNCTION_CANDIDATE" if abi == "arm64-v8a" else "LEGACY_CPU_PIXELS_VULKAN_NN",
        device_qualified=False)
    (OUT / (abi + "-receipt.json")).write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")

if not ARGS.objects_only and ARGS.jvm_api:
    if ARGS.abi != "all":
        raise ValueError("Finalization requires both ABI builds from the same invocation")
    with zipfile.ZipFile(ARGS.jvm_api) as archive:
        if any(n.startswith("jni/") or n.endswith(".so") for n in archive.namelist()):
            raise ValueError("JVM API AAR must not own native binaries")
    api = OUT / "meguviz-media-api-release.aar"
    shutil.copyfile(ARGS.jvm_api, api)
    sources = {name: source_identity(name) for name in ("MeguvizCompute", "MeguvizMedia", "MeguvizRIFE", "MeguvizShaderTools")}
    shader_files = sorted((PACK / "MeguvizRIFE/src/backends/ncnn/legacy").rglob("*.comp"))
    shader_files += sorted((PACK / "MeguvizCompute/schemas/shaderpack").rglob("*"))
    shader_inputs = [{"path": str(p.relative_to(PACK)).replace("\\", "/"), "sha256": sha(p)} for p in shader_files if p.is_file()]
    fingerprint = hashlib.sha256(json.dumps(shader_inputs, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    notice = None
    if ARGS.openmp == "on":
        notice_source = ARGS.ndk / "toolchains/llvm/prebuilt/windows-x86_64/NOTICE"
        hashes = {info["android_openmp_notice_sha256"] for info in build_infos.values()}
        if len(hashes) != 1 or sha(notice_source) not in hashes:
            raise RuntimeError("The packaged LLVM notice does not match both ABI builds")
        notice_file = OUT / "notices/llvm-openmp-NOTICE.txt"
        notice_file.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(notice_source, notice_file)
        notice = dict(path=str(notice_file.relative_to(OUT)).replace("\\", "/"), sha256=sha(notice_file), size_bytes=notice_file.stat().st_size)
    lock = OUT / "composition-lock.json"
    lock.write_text(json.dumps(dict(schema_version=1, sources=sources, shader_inputs=shader_inputs,
        shaderpack_fingerprint=fingerprint, runtime_compiler_retained=True,
        toolchain=dict(ndk=str(ARGS.ndk), cmake=str(ARGS.cmake), stl="c++_static", api=27,
                       android_static_openmp=ARGS.openmp.upper(),
                       job_observation=ARGS.job_observation.upper(),
                       shared_plan=ARGS.shared_plan.upper(),
                       openmp_runtime_sha256={abi: info["android_openmp_runtime_sha256"] for abi, info in build_infos.items()},
                       openmp_notice=notice),
        qualification="FUNCTION_CANDIDATE_DEVICE_GATES_OPEN"), indent=2) + "\n", encoding="utf-8")
    entries = [json.loads((OUT / (abi + "-receipt.json")).read_text()) for abi in abis]
    manifest = dict(schema_version=1, release_profile="bil-rife-ncnn-android", finalized=True,
        composition_lock=dict(path=lock.name, sha256=sha(lock)), shaderpack_fingerprint=fingerprint,
        runtime_compiler_retained=True, jvm_api=dict(path=api.name, sha256=sha(api), size_bytes=api.stat().st_size),
        abis=entries, candidate_only=True, release_qualified=False, device_qualified=False, gpu_qualified=False,
        packaging_modes_exclusive=["jni-AAR", "native-payload+JVM-API"], native_owner="Media",
        notices=[notice] if notice else [])
    (OUT / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
