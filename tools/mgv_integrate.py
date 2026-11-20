"""Inspect, source-check or build an isolated development composition.

The package check covers specified entries and known native-owner names; it
does not establish unique runtime ownership or discover renamed or statically
embedded libraries. Device behavior, ELF compatibility, full dependency/lock
completeness and release qualification are outside these commands' scope.
No packages are installed or modified.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import zipfile

ABIS = {"arm64-v8a", "armeabi-v7a"}
LIBRARY = "libmeguviz_media_rife_ncnn.so"
JNI_CLASS = "io.github.jichuo1.meguviz.media.MediaNative"
JNI_CLASS_ENTRY = JNI_CLASS.replace(".", "/") + ".class"
CONFLICTING_LIBRARIES = {"libinnocent_frame_interpolation.so", "libncnn.so"}
MAX_LIBRARY = 16 * 1024 * 1024
MAX_LOADER_ARCHIVE = 8 * 1024 * 1024
MAX_PACKAGE = 256 * 1024 * 1024
PROJECTS = ("MeguvizCompute", "MeguvizMedia", "MeguvizRIFE", "MeguvizShaderTools")
IGNORED_SOURCE_NAMES = {"STATUS.json", "ARTIFACTS.json", "SOURCE_MANIFEST.json"}
IGNORED_SOURCE_PREFIXES = ("artifacts/", "build/", "android/.gradle/")


class UnsupportedLockError(ValueError):
    """The supplied release or planning format has no implemented verifier."""


def sha(data):
    return hashlib.sha256(data).hexdigest()


def unique_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key: " + key)
        result[key] = value
    return result


def json_bytes(data):
    return json.loads(data.decode("utf-8-sig"), object_pairs_hook=unique_pairs,
        parse_constant=lambda value: (_ for _ in ()).throw(ValueError("nonfinite JSON value")))


def check_hash(value):
    return isinstance(value, str) and len(value) == 64 and all(c in "0123456789abcdef" for c in value)


def canonical_hash(value):
    return sha(json.dumps(value, sort_keys=True, separators=(",", ":")).encode())


def composition_size_profiles(toolchain):
    if not isinstance(toolchain, dict):
        raise ValueError("invalid composition toolchain")
    layers = toolchain.get("ncnn_layer_profile", "full")
    relocations = toolchain.get("android_packed_relocations", "OFF")
    if layers not in ("full", "rife46", "rife46-arm64") or relocations not in ("ON", "OFF"):
        raise ValueError("unsupported ncnn layer or relocation profile in the composition lock")
    return {"ncnn_layer_profile": layers, "android_packed_relocations": relocations}


def require_composition_size_profiles(toolchain, record, abi=None):
    profiles = composition_size_profiles(toolchain)
    for name, value in profiles.items():
        if (name in toolchain or name in record) and record.get(name) != value:
            raise ValueError("ABI composition profile differs from the lock: " + name)
    if abi is not None:
        expected = profiles["ncnn_layer_profile"]
        mixed = expected == "rife46-arm64"
        if mixed:
            expected = "rife46" if abi == "arm64-v8a" else "full"
        if mixed or "ncnn_effective_layer_profile" in record:
            if record.get("ncnn_effective_layer_profile") != expected:
                raise ValueError("ABI composition profile differs from the lock: ncnn_effective_layer_profile")


def composition_size_arguments(builder, toolchain):
    profiles = composition_size_profiles(toolchain)
    if not any(name in toolchain for name in profiles):
        # Legacy locks predating both options bind full layers and plain
        # relocations. Older locked builders already used those defaults.
        help_result = subprocess.run([sys.executable, "-B", str(builder), "--help"],
            cwd=builder.parent.parent, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        if help_result.returncode:
            raise ValueError("could not determine the locked composition builder options")
        if b"--ncnn-layer-profile" not in help_result.stdout and b"--packed-relocations" not in help_result.stdout:
            return []
    return ["--ncnn-layer-profile", profiles["ncnn_layer_profile"],
            "--packed-relocations", profiles["android_packed_relocations"].lower()]


def git_output(directory, *arguments):
    command = ["git", "-c", "safe.directory=" + str(directory), "-C", str(directory), *arguments]
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if result.returncode:
        raise ValueError("source identity command failed: " + str(directory))
    return result.stdout


def source_identity(directory, exclude_generated=True):
    directory = Path(directory).resolve(strict=True)
    revision = git_output(directory, "rev-parse", "HEAD").decode("ascii", "strict").strip()
    if len(revision) not in (40, 64) or any(c not in "0123456789abcdef" for c in revision):
        raise ValueError("invalid source revision: " + str(directory))
    raw = git_output(directory, "ls-files", "-z", "--cached", "--others", "--exclude-standard")
    names = [name.decode("utf-8", "strict") for name in raw.split(b"\0") if name]
    if len(names) != len(set(names)):
        raise ValueError("duplicate source paths: " + str(directory))
    files = []
    for name in names:
        if exclude_generated and (name in IGNORED_SOURCE_NAMES or name.startswith(IGNORED_SOURCE_PREFIXES)):
            continue
        if name.startswith("/") or "\\" in name or any(part in ("", ".", "..") for part in name.split("/")):
            raise ValueError("invalid source path: " + name)
        path = (directory / name).resolve()
        if not path.is_relative_to(directory):
            raise ValueError("source escapes checkout: " + name)
        if path.is_file():
            files.append({"path": name, "sha256": sha(path.read_bytes())})
    return {"revision": revision, "source_tree_sha256": canonical_hash(files), "files": files}


def verify_development_lock(lock_path, source_root):
    lock_path = Path(lock_path).resolve(strict=True)
    if lock_path.stat().st_size > 32 * 1024 * 1024:
        raise ValueError("source lock is oversized")
    lock_data = lock_path.read_bytes()
    lock = json_bytes(lock_data)
    if not isinstance(lock, dict) or lock.get("schema_version") != 1 or not isinstance(lock.get("sources"), dict):
        raise UnsupportedLockError("unsupported or incomplete development composition lock; release lock is not implemented")
    if set(lock["sources"]) != set(PROJECTS):
        raise ValueError("development lock must bind all four source projects")
    source_root = Path(source_root).resolve(strict=True)
    summary = {}
    for project in PROJECTS:
        expected = lock["sources"][project]
        if (not isinstance(expected, dict) or not isinstance(expected.get("files"), list)
                or not check_hash(expected.get("source_tree_sha256"))
                or canonical_hash(expected["files"]) != expected["source_tree_sha256"]):
            raise ValueError("invalid locked source tree digest: " + project)
        actual = source_identity(source_root / project)
        if actual != {key: expected.get(key) for key in ("revision", "source_tree_sha256", "files")}:
            raise ValueError("live source differs from development lock: " + project)
        summary[project] = {"revision": actual["revision"], "source_tree_sha256": actual["source_tree_sha256"],
                            "files_checked": len(actual["files"])}
    shader_paths = sorted((source_root / "MeguvizRIFE/src/backends/ncnn/legacy").rglob("*.comp"))
    shader_paths += sorted((source_root / "MeguvizCompute/schemas/shaderpack").rglob("*"))
    shader_inputs = [{"path": p.relative_to(source_root).as_posix(), "sha256": sha(p.read_bytes())}
                     for p in shader_paths if p.is_file()]
    if (shader_inputs != lock.get("shader_inputs") or canonical_hash(shader_inputs) != lock.get("shaderpack_fingerprint")):
        raise ValueError("shader inputs differ from development lock")
    if lock.get("runtime_compiler_retained") is not True or not isinstance(lock.get("toolchain"), dict):
        raise ValueError("development build configuration is incomplete")
    composition_size_profiles(lock["toolchain"])
    return {"schema_version": 1, "result": "PASS", "scope": "DEVELOPMENT_PROJECT_SOURCE_LOCK_ONLY",
            "lock": {"path": str(lock_path), "sha256": sha(lock_data)}, "source_root": str(source_root),
            "projects": summary, "shader_inputs_checked": len(shader_inputs),
            "dependency_source_lock": "NOT_INCLUDED", "tool_binary_lock": "NOT_INCLUDED",
            "release_qualification": "NOT_EVALUATED"}


def material(root, entry, limit=MAX_PACKAGE):
    if not isinstance(entry, dict) or not isinstance(entry.get("path"), str) or not check_hash(entry.get("sha256")):
        raise ValueError("invalid material descriptor")
    path = (root / entry["path"]).resolve(strict=True)
    if not path.is_relative_to(root) or not path.is_file():
        raise ValueError("material outside composition")
    size = path.stat().st_size
    if size > limit or ("size_bytes" in entry and (type(entry["size_bytes"]) is not int or entry["size_bytes"] != size)):
        raise ValueError("material size mismatch or limit exceeded: " + entry["path"])
    data = path.read_bytes()
    if sha(data) != entry["sha256"]:
        raise ValueError("material hash mismatch: " + entry["path"])
    return data


def entries(archive):
    names = archive.namelist()
    if len(names) != len(set(names)):
        raise ValueError("duplicate ZIP entry")
    for item in archive.infolist():
        name = item.filename
        parts = name.split("/")
        if (not name or name.startswith("/") or "\\" in name or any(part in (".", "..") for part in parts)
                or item.file_size > MAX_PACKAGE):
            raise ValueError("unsafe or oversized ZIP entry: " + name)
    return names


def exact_archives(data, expected, prefix):
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        names = entries(archive)
        libraries = [name for name in names if name.lower().endswith(".so")]
        wanted = {prefix + abi + "/" + LIBRARY for abi in ABIS}
        if set(libraries) != wanted:
            raise ValueError("native owners or ABI set differ: " + str(libraries))
        for abi, binary in expected.items():
            info = archive.getinfo(prefix + abi + "/" + LIBRARY)
            if info.file_size > MAX_LIBRARY or archive.read(info) != binary:
                raise ValueError("native bytes mismatch: " + abi)
        return names


def validate_aar_jars(archive, label):
    jar_names = [name for name in archive.namelist() if name.lower().endswith(".jar")]
    if "classes.jar" not in jar_names:
        raise ValueError(label + " is missing classes.jar")
    total = 0
    for jar_name in jar_names:
        info = archive.getinfo(jar_name)
        total += info.file_size
        if total > MAX_PACKAGE:
            raise ValueError(label + " nested JARs exceed limit")
        with zipfile.ZipFile(io.BytesIO(archive.read(info))) as classes:
            names = entries(classes)
            if sum(entry.file_size for entry in classes.infolist()) > MAX_PACKAGE:
                raise ValueError(label + " expanded JAR exceeds limit")
            if jar_name == "classes.jar" and JNI_CLASS_ENTRY not in names:
                raise ValueError(label + " classes.jar is missing MediaNative class")
            if any(name.lower().endswith((".so", ".dll", ".dylib")) for name in names):
                raise ValueError(label + " JAR contains nested native library")
            damaged = classes.testzip()
            if damaged is not None:
                raise ValueError(label + " JAR contains corrupt entry: " + damaged)


def conflicting_native_owner(name):
    basename = name.rsplit("/", 1)[-1].lower()
    return basename in CONFLICTING_LIBRARIES or (basename.startswith("libmeguviz_") and basename.endswith(".so"))


def inspect(manifest_path, bil_apk=None):
    manifest_path = Path(manifest_path).resolve(strict=True)
    if manifest_path.stat().st_size > 4 * 1024 * 1024:
        raise ValueError("manifest is oversized")
    manifest_data = manifest_path.read_bytes()
    manifest = json_bytes(manifest_data)
    if (not isinstance(manifest, dict) or manifest.get("schema_version") != 1 or manifest.get("finalized") is not True
            or manifest.get("release_profile") != "bil-rife-ncnn-android" or manifest.get("native_owner") != "Media"):
        raise ValueError("unsupported or unfinished composition manifest")
    root = manifest_path.parent
    lock_data = material(root, manifest.get("composition_lock"))
    lock = json_bytes(lock_data)
    if (not isinstance(lock, dict) or not check_hash(manifest.get("shaderpack_fingerprint"))
            or lock.get("shaderpack_fingerprint") != manifest.get("shaderpack_fingerprint")):
        raise ValueError("composition lock shader fingerprint mismatch")
    toolchain = lock.get("toolchain", {})
    composition_size_profiles(toolchain)
    observation = toolchain.get("job_observation", "OFF")
    if observation not in ("ON", "OFF"):
        raise ValueError("invalid job observation build profile")
    shared_plan = lock.get("toolchain", {}).get("shared_plan")
    if shared_plan is not None and shared_plan not in ("ON", "OFF"):
        raise ValueError("invalid shared plan build profile")
    api_data = material(root, manifest.get("jvm_api"))
    with zipfile.ZipFile(io.BytesIO(api_data)) as archive:
        names = entries(archive)
        if not {"AndroidManifest.xml", "classes.jar"}.issubset(names):
            raise ValueError("JVM API AAR is missing required entries")
        if any(name.lower().endswith(".so") or name.lower().startswith("jni/") for name in names):
            raise ValueError("JVM API AAR contains native ownership")
        validate_aar_jars(archive, "JVM API AAR")
    abis = manifest.get("abis")
    if not isinstance(abis, list) or len(abis) != 2 or {a.get("abi") for a in abis if isinstance(a, dict)} != ABIS:
        raise ValueError("both distinct Android ABIs are required")
    expected = {}
    for item in abis:
        require_composition_size_profiles(toolchain, item, item["abi"])
        if not check_hash(item.get("private_sdk_fingerprint")):
            raise ValueError("private SDK fingerprint missing")
        if item.get("job_observation", "OFF") != observation:
            raise ValueError("ABI job observation build profile mismatch")
        if shared_plan is not None and item.get("shared_plan") != shared_plan:
            raise ValueError("ABI shared plan build profile mismatch")
        if observation == "ON" and not check_hash(item.get("rife_build_fingerprint")):
            raise ValueError("RIFE observation build fingerprint missing")
        expected[item["abi"]] = material(root, item, MAX_LIBRARY)
    payload = material(root, manifest.get("native_payload_archive"))
    exact_archives(payload, expected, "")
    jni = material(root, manifest.get("jni_aar"))
    names = exact_archives(jni, expected, "jni/")
    if not {"AndroidManifest.xml", "classes.jar"}.issubset(names):
        raise ValueError("JNI AAR is missing required entries")
    with zipfile.ZipFile(io.BytesIO(jni)) as archive:
        validate_aar_jars(archive, "JNI AAR")
    if any(name.startswith("assets/") and ("native-payload" in name or name.endswith("VisionTool.zip")) for name in names):
        raise ValueError("JNI AAR also carries native payload owner")
    sample = material(root, manifest.get("sample_apk"))
    names = exact_archives(sample, expected, "lib/")
    if any(name.startswith("assets/") and ("native-payload" in name or name.endswith("VisionTool.zip")) for name in names):
        raise ValueError("sample APK contains duplicate native owner")
    for entry in manifest.get("notices", []):
        material(root, entry)
    if not manifest.get("notices"):
        raise ValueError("license notices missing")
    if lock.get("toolchain", {}).get("android_static_openmp") == "ON":
        llvm = [item for item in manifest["notices"] if item.get("path") == "notices/llvm-openmp-NOTICE.txt"]
        if len(llvm) != 1:
            raise ValueError("OpenMP-enabled composition must carry exactly one LLVM notice")
        if lock.get("toolchain", {}).get("openmp_notice") != llvm[0]:
            raise ValueError("OpenMP notice differs from composition lock")
        notice = material(root, llvm[0])
        for data, label, name in ((payload, "native payload", "META-INF/llvm-openmp-NOTICE.txt"),
                                  (jni, "JNI AAR", "assets/licenses/llvm-openmp-NOTICE.txt"),
                                  (sample, "sample APK", "assets/licenses/llvm-openmp-NOTICE.txt")):
            with zipfile.ZipFile(io.BytesIO(data)) as archive:
                if archive.namelist().count(name) != 1 or archive.read(name) != notice:
                    raise ValueError(label + " OpenMP notice mismatch")
    checked_bil = None
    if bil_apk is not None:
        apk = Path(bil_apk).resolve(strict=True)
        if not apk.is_file() or apk.stat().st_size > MAX_PACKAGE:
            raise ValueError("invalid or oversized bil APK")
        apk_bytes = apk.read_bytes()
        with zipfile.ZipFile(io.BytesIO(apk_bytes)) as archive:
            names = entries(archive)
            if lock.get("toolchain", {}).get("android_static_openmp") == "ON":
                notice_asset = "assets/licenses/frame-interpolation/llvm-openmp-NOTICE.txt"
                if names.count(notice_asset) != 1 or archive.read(notice_asset) != notice:
                    raise ValueError("bil APK OpenMP notice mismatch")
            expected_loader = {"assets/native-packing/" + abi + "/" + name
                for abi in ABIS for name in ("VisionTool.zip", "receipt.json")}
            actual_loader = {name for name in names if name.startswith("assets/native-packing/")
                and (name.endswith("/VisionTool.zip") or name.endswith("/receipt.json"))}
            if actual_loader != expected_loader:
                raise ValueError("bil loader assets or ABI set differ")
            conflicts = [name for name in names if conflicting_native_owner(name)]
            if conflicts:
                raise ValueError("bil APK has both loader payload and conflicting native owner: " + str(conflicts))
            for item in abis:
                abi = item["abi"]
                base = "assets/native-packing/" + abi + "/"
                info = archive.getinfo(base + "VisionTool.zip")
                if info.file_size > MAX_LOADER_ARCHIVE:
                    raise ValueError("bil loader archive exceeds limit: " + abi)
                payload_bytes = archive.read(info)
                receipt_info = archive.getinfo(base + "receipt.json")
                if receipt_info.file_size > 1024 * 1024:
                    raise ValueError("bil receipt exceeds limit")
                receipt = json_bytes(archive.read(receipt_info))
                if (receipt.get("abi") != abi or receipt.get("library_name") != LIBRARY or receipt.get("jni_class") != JNI_CLASS
                        or receipt.get("library_sha256") != sha(expected[abi]) or receipt.get("library_bytes") != len(expected[abi])
                        or receipt.get("archive_sha256") != sha(payload_bytes) or receipt.get("archive_bytes") != len(payload_bytes)
                        or receipt.get("composition_lock_sha256") != sha(lock_data)
                        or receipt.get("private_sdk_fingerprint") != item["private_sdk_fingerprint"]
                        or receipt.get("job_observation", "OFF") != observation
                        or receipt.get("shaderpack_fingerprint") != manifest.get("shaderpack_fingerprint")
                        or receipt.get("canonical_bytes_preserved") is not True
                        or (lock.get("toolchain", {}).get("android_static_openmp") == "ON"
                            and receipt.get("openmp_notice_sha256") != sha(notice))):
                    raise ValueError("bil receipt differs from canonical composition: " + abi)
                with zipfile.ZipFile(io.BytesIO(payload_bytes)) as nested:
                    nested_names = entries(nested)
                    target = "lib/" + abi + "/" + LIBRARY
                    if nested_names != [target] or nested.getinfo(target).file_size > MAX_LIBRARY or nested.read(target) != expected[abi]:
                        raise ValueError("bil payload bytes or owner mismatch: " + abi)
        checked_bil = {"path": str(apk), "sha256": sha(apk_bytes), "size_bytes": len(apk_bytes)}
    return {"schema_version": 1, "result": "PASS", "scope": "ANDROID_PACKAGE_IDENTITY_ONLY",
        "manifest": {"path": str(manifest_path), "sha256": sha(manifest_data)},
        "composition_lock_sha256": sha(lock_data), "abis": sorted(ABIS),
        "jvm_api_native_free": True, "jni_aar_canonical": True, "native_payload_canonical": True,
        "sample_apk_canonical": True, "notices_verified": len(manifest["notices"]),
        "bil_apk": checked_bil if checked_bil else "NOT_PROVIDED",
        "native_owner_name_checks": ["libinnocent_frame_interpolation.so", "libncnn.so", "libmeguviz_*.so"],
        "runtime_owner_uniqueness": "NOT_PROVEN",
        "inspection_limitations": [
            "Known APK entry names and designated loader archives only; renamed DSOs and statically embedded libraries are not detected.",
            "JVM API native-free status covers inspected AAR entries and immediate JAR entries; arbitrary deeper nested archives are not evaluated.",
            "JAR structure and entry integrity are checked; Java bytecode semantics are not evaluated."
        ],
        "device_qualification": "NOT_EVALUATED", "elf_qualification": "NOT_EVALUATED",
        "release_qualification": "NOT_EVALUATED"}


def build_candidate(lock_path, source_root, api_path, api_sha256, output_path, openmp="on", job_observation="off"):
    """Build an isolated development payload with observed before/after inputs."""
    source_root = Path(source_root).resolve(strict=True)
    output_path = Path(output_path).resolve()
    protected_roots = [(source_root / name).resolve() for name in (*PROJECTS, "dependencies")]
    if (not output_path.is_relative_to(source_root) or output_path == source_root
            or any(output_path.is_relative_to(path) for path in protected_roots)
            or output_path.exists() or not output_path.parent.is_dir()):
        raise ValueError("build output must be a new directory within the workspace, outside source projects and dependencies")
    if not check_hash(api_sha256):
        raise ValueError("the expected API AAR SHA256 is required")
    api_path = Path(api_path).resolve(strict=True)
    if not api_path.is_file() or api_path.stat().st_size > MAX_PACKAGE or sha(api_path.read_bytes()) != api_sha256:
        raise ValueError("API AAR identity mismatch")
    with zipfile.ZipFile(api_path) as archive:
        names = entries(archive)
        if not {"AndroidManifest.xml", "classes.jar"}.issubset(names) or any(n.lower().startswith("jni/") or n.lower().endswith(".so") for n in names):
            raise ValueError("API AAR must be complete and native-free")
        validate_aar_jars(archive, "API AAR")
    lock = json_bytes(Path(lock_path).read_bytes())
    preflight = verify_development_lock(lock_path, source_root)
    tc = lock["toolchain"]
    if tc.get("api") != 27 or tc.get("stl") != "c++_static":
        raise ValueError("unsupported development Android API or STL profile")
    if openmp not in ("on", "off") or (tc.get("android_static_openmp") is not None and tc["android_static_openmp"] != openmp.upper()):
        raise ValueError("requested OpenMP profile differs from the development lock")
    if job_observation not in ("on", "off") or tc.get("job_observation", "OFF") != job_observation.upper():
        raise ValueError("requested job observation profile differs from the development lock")
    shared_plan = tc.get("shared_plan", "ON")
    if shared_plan not in ("ON", "OFF"):
        raise ValueError("unsupported shared plan profile in the development lock")
    builder = source_root / "MeguvizMedia/tools/build_android_composition.py"
    size_arguments = composition_size_arguments(builder, tc)
    ndk = Path(tc.get("ndk", "")).resolve(strict=True)
    cmake = Path(tc.get("cmake", "")).resolve(strict=True)
    ninja = cmake.parent / ("ninja.exe" if sys.platform == "win32" else "ninja")
    prebuilt = ndk / "toolchains/llvm/prebuilt/windows-x86_64/bin"
    if sys.platform != "win32":
        raise ValueError("isolated development builder currently supports the pinned Windows toolchain only")
    tools = {"cmake": cmake, "ninja": ninja, "clang": prebuilt / "clang++.exe", "strip": prebuilt / "llvm-strip.exe",
             "ndk_properties": ndk / "source.properties"}
    if any(not path.is_file() for path in tools.values()):
        raise ValueError("development toolchain files are unavailable")
    tool_hashes = {name: sha(path.read_bytes()) for name, path in tools.items()}
    dependencies = {"ncnn": source_root / "dependencies/ncnn-meguviz",
                    "glslang": source_root / "dependencies/glslang",
                    "ncnn_glslang": source_root / "dependencies/ncnn-meguviz/glslang"}
    dependency_before = {name: source_identity(path, False) for name, path in dependencies.items()}
    output_path.mkdir()
    record = {"schema_version": 1, "result": "IN_PROGRESS", "scope": "ISOLATED_DEVELOPMENT_BUILD_ONLY",
              "profile": "bil-native-payload", "lock": preflight["lock"], "project_sources": preflight["projects"],
              "api_aar": {"path": str(api_path), "sha256": api_sha256, "size_bytes": api_path.stat().st_size},
              "dependencies_observed_before": {name: {"revision": identity["revision"], "source_tree_sha256": identity["source_tree_sha256"],
                  "files_checked": len(identity["files"])} for name, identity in dependency_before.items()},
              "tool_sha256": tool_hashes, "dependency_source_lock": "NOT_INCLUDED_IN_DEVELOPMENT_LOCK",
              "tool_binary_lock": "NOT_INCLUDED_IN_DEVELOPMENT_LOCK", "reproducible_bytes": "NOT_VERIFIED",
              "device_qualification": "NOT_EVALUATED", "release_qualification": "NOT_EVALUATED"}
    record_path = output_path / "build-record.json"
    def save():
        record_path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    save()
    command = [sys.executable, "-B", str(builder),
        "--abi", "all", "--ndk", str(ndk), "--cmake", str(cmake), "--ninja", str(ninja),
        "--jvm-api", str(api_path), "--openmp", openmp, "--job-observation", job_observation,
        "--shared-plan", shared_plan.lower(), "--output-dir", str(output_path / "composition"),
        "--build-root", str(output_path / "build"), *size_arguments]
    record["argv"] = command
    save()
    try:
        with (output_path / "build.stdout").open("xb") as stdout, (output_path / "build.stderr").open("xb") as stderr:
            result = subprocess.run(command, cwd=source_root / "MeguvizMedia", stdout=stdout, stderr=stderr, check=False)
        record["exit_code"] = result.returncode
        record["stdout_sha256"] = sha((output_path / "build.stdout").read_bytes())
        record["stderr_sha256"] = sha((output_path / "build.stderr").read_bytes())
        if result.returncode:
            raise ValueError("isolated Android composition build failed; logs retained")
        postflight = verify_development_lock(lock_path, source_root)
        if postflight["lock"]["sha256"] != preflight["lock"]["sha256"]:
            raise ValueError("development lock changed during build")
        if sha(api_path.read_bytes()) != api_sha256:
            raise ValueError("API AAR changed during build")
        if {name: sha(path.read_bytes()) for name, path in tools.items()} != tool_hashes:
            raise ValueError("tool binary changed during build")
        for name, path in dependencies.items():
            if source_identity(path, False) != dependency_before[name]:
                raise ValueError("dependency source changed during build: " + name)
        composition = output_path / "composition"
        manifest_path = composition / "manifest.json"
        manifest = json_bytes(manifest_path.read_bytes())
        if not manifest.get("finalized") or {a["abi"] for a in manifest.get("abis", [])} != ABIS:
            raise ValueError("builder did not produce a complete dual-ABI composition")
        produced_lock = json_bytes(material(composition, manifest["composition_lock"]))
        if produced_lock.get("sources") != lock.get("sources") or produced_lock.get("shader_inputs") != lock.get("shader_inputs"):
            raise ValueError("produced source lock differs from input source lock")
        if produced_lock.get("toolchain", {}).get("shared_plan") != shared_plan:
            raise ValueError("produced shared plan profile differs from the development lock")
        expected_size_profiles = dict(tc)
        if size_arguments:
            expected_size_profiles.update(composition_size_profiles(tc))
        require_composition_size_profiles(expected_size_profiles, produced_lock.get("toolchain", {}))
        for item in manifest["abis"]:
            require_composition_size_profiles(expected_size_profiles, item, item["abi"])
        if sha(material(composition, manifest["jvm_api"])) != api_sha256:
            raise ValueError("produced API AAR differs from locked input")
        payload = output_path / "native-payload.zip"
        with zipfile.ZipFile(payload, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for item in sorted(manifest["abis"], key=lambda x: x["abi"]):
                binary = material(composition, item, MAX_LIBRARY)
                info = zipfile.ZipInfo(item["abi"] + "/" + LIBRARY, date_time=(1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(info, binary)
            notices = [item for item in manifest.get("notices", []) if item.get("path") == "notices/llvm-openmp-NOTICE.txt"]
            if len(notices) > 1:
                raise ValueError("duplicate OpenMP notice")
            if notices:
                archive.writestr("META-INF/llvm-openmp-NOTICE.txt", material(composition, notices[0]))
        record.update(result="PASS", source_and_dependency_inputs_unchanged=True,
            manifest={"path": str(manifest_path), "sha256": sha(manifest_path.read_bytes())},
            native_payload={"path": str(payload), "sha256": sha(payload.read_bytes()), "size_bytes": payload.stat().st_size},
            qualification="INCONCLUSIVE")
    except (ValueError, OSError, KeyError, TypeError, zipfile.BadZipFile) as error:
        record.update(result="FAIL", error=str(error))
    save()
    return record


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    inspect_parser = commands.add_parser("inspect-package", help="check the exact Android composition and optional bil loader package")
    inspect_parser.add_argument("--manifest", type=Path, required=True)
    inspect_parser.add_argument("--bil-apk", type=Path)
    inspect_parser.add_argument("--out", type=Path, help="new JSON report path; existing files are preserved")
    verify_parser = commands.add_parser("verify-lock", help="verify the actual four-project development source lock")
    verify_parser.add_argument("--lock", type=Path, required=True)
    verify_parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[2])
    verify_parser.add_argument("--out", type=Path, help="new JSON report path")
    build_parser = commands.add_parser("build", help="build a new isolated development native payload")
    build_parser.add_argument("--profile", required=True, choices=("bil-native-payload",))
    build_parser.add_argument("--lock", type=Path, required=True)
    build_parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[2])
    build_parser.add_argument("--jvm-api", type=Path, required=True)
    build_parser.add_argument("--jvm-api-sha256", required=True)
    build_parser.add_argument("--openmp", choices=("on", "off"), default="on", help="Android static OpenMP profile; off selects the fallback")
    build_parser.add_argument("--job-observation", choices=("on", "off"), default="off", help="Optional result-owned execution observations")
    build_parser.add_argument("--out", type=Path, required=True, help="new build directory outside the source projects")
    args = parser.parse_args(argv)
    try:
        if args.command == "inspect-package":
            result = inspect(args.manifest, args.bil_apk)
        elif args.command == "verify-lock":
            result = verify_development_lock(args.lock, args.source_root)
        else:
            result = build_candidate(args.lock, args.source_root, args.jvm_api, args.jvm_api_sha256, args.out, args.openmp, args.job_observation)
    except UnsupportedLockError as error:
        result = {"schema_version": 1, "result": "UNSUPPORTED", "scope": "DEVELOPMENT_PROJECT_SOURCE_LOCK_ONLY",
                  "error": str(error), "device_qualification": "NOT_EVALUATED", "release_qualification": "NOT_EVALUATED"}
    except (ValueError, KeyError, TypeError, OSError, zipfile.BadZipFile) as error:
        scope = {"inspect-package": "ANDROID_PACKAGE_IDENTITY_ONLY", "verify-lock": "DEVELOPMENT_PROJECT_SOURCE_LOCK_ONLY",
                 "build": "ISOLATED_DEVELOPMENT_BUILD_ONLY"}[args.command]
        result = {"schema_version": 1, "result": "FAIL", "scope": scope, "error": str(error),
                  "device_qualification": "NOT_EVALUATED", "release_qualification": "NOT_EVALUATED"}
    rendered = json.dumps(result, indent=2) + "\n"
    if args.command != "build" and args.out:
        with args.out.open("x", encoding="utf-8") as stream:
            stream.write(rendered)
    print(rendered, end="")
    return 0 if result["result"] == "PASS" else 3 if result["result"] == "UNSUPPORTED" else 2


if __name__ == "__main__":
    sys.exit(main())
