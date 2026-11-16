"""Focused synthetic checks for the offline package identity inspector."""
import hashlib
import base64
import importlib.util
import io
import json
from pathlib import Path
import sys
import time
import unittest
from unittest.mock import patch
import warnings
import zipfile

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "MeguvizMedia/tools/mgv_integrate.py"
spec = importlib.util.spec_from_file_location("mgv_integrate", MODULE)
inspector = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inspector)
BASE = ROOT / "coordination/verification" / ("mgv-integrate-fixtures-" + str(time.time_ns()))
BASE.mkdir(parents=True)
CLASS_BYTES = base64.b64decode(
    "yv66vgAAADQADQoAAgADBwAEDAAFAAYBABBqYXZhL2xhbmcvT2JqZWN0AQAGPGluaXQ+AQADKClWBwAIAQAraW8vZ2l0aHViL2ppY2h1bzEvbWVndXZpei9tZWRpYS9NZWRpYU5hdGl2ZQEABENvZGUBAA9MaW5lTnVtYmVyVGFibGUBAApTb3VyY2VGaWxlAQAQTWVkaWFOYXRpdmUuamF2YQAxAAcAAgAAAAAAAQABAAUABgABAAkAAAAdAAEAAQAAAAUqtwABsQAAAAEACgAAAAYAAQAAAAEAAQALAAAAAgAM")


def digest(data): return hashlib.sha256(data).hexdigest()


def zipped(entries):
    result = io.BytesIO()
    with zipfile.ZipFile(result, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", UserWarning)
            for name, data in entries:
                archive.writestr(name, data)
    return result.getvalue()


def fixture(name):
    root = BASE / name
    root.mkdir()
    def save(path, data):
        target = root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        return {"path": path, "sha256": digest(data), "size_bytes": len(data)}
    shader = "b" * 64
    lock = json.dumps({"schema_version": 1, "shaderpack_fingerprint": shader}).encode()
    lock_desc = save("composition-lock.json", lock)
    native = {abi: ("binary-" + abi).encode() for abi in sorted(inspector.ABIS)}
    items = []
    for abi, binary in native.items():
        item = save("native-payload/" + abi + "/" + inspector.LIBRARY, binary)
        item.update(abi=abi, private_sdk_fingerprint=("a" if abi == "arm64-v8a" else "c") * 64)
        items.append(item)
    jar = zipped([("META-INF/MANIFEST.MF", b"Manifest-Version: 1.0\r\n\r\n"), (inspector.JNI_CLASS_ENTRY, CLASS_BYTES)])
    base = [("AndroidManifest.xml", b"xml"), ("classes.jar", jar)]
    api = save("api.aar", zipped(base))
    jni = save("jni.aar", zipped(base + [("jni/" + abi + "/" + inspector.LIBRARY, data) for abi, data in native.items()]))
    sample = save("sample.apk", zipped([("lib/" + abi + "/" + inspector.LIBRARY, data) for abi, data in native.items()]))
    payload = save("payload.zip", zipped([(abi + "/" + inspector.LIBRARY, data) for abi, data in native.items()]))
    notice = save("notice.txt", b"license")
    manifest = {"schema_version": 1, "release_profile": "bil-rife-ncnn-android", "finalized": True,
        "native_owner": "Media", "composition_lock": lock_desc, "shaderpack_fingerprint": shader,
        "jvm_api": api, "abis": items, "native_payload_archive": payload, "jni_aar": jni,
        "sample_apk": sample, "notices": [notice]}
    path = root / "manifest.json"
    path.write_text(json.dumps(manifest), encoding="utf-8")
    apk_entries = []
    for item in items:
        abi = item["abi"]
        nested = zipped([("lib/" + abi + "/" + inspector.LIBRARY, native[abi])])
        receipt = {"abi": abi, "library_name": inspector.LIBRARY, "jni_class": inspector.JNI_CLASS,
            "library_sha256": digest(native[abi]), "library_bytes": len(native[abi]),
            "archive_sha256": digest(nested), "archive_bytes": len(nested),
            "composition_lock_sha256": digest(lock), "private_sdk_fingerprint": item["private_sdk_fingerprint"],
            "shaderpack_fingerprint": shader, "canonical_bytes_preserved": True}
        base_path = "assets/native-packing/" + abi + "/"
        apk_entries.extend([(base_path + "VisionTool.zip", nested), (base_path + "receipt.json", json.dumps(receipt).encode())])
    apk = root / "bil.apk"
    apk.write_bytes(zipped(apk_entries))
    return root, path, manifest, apk, apk_entries


class InspectTests(unittest.TestCase):
    def test_declared_shared_plan_profile_matches_both_abis(self):
        root, path, manifest, _, _ = fixture("shared-plan-mismatch")
        lock = json.loads((root / "composition-lock.json").read_bytes())
        lock["toolchain"] = {"shared_plan": "OFF"}
        lock_bytes = json.dumps(lock).encode()
        (root / "composition-lock.json").write_bytes(lock_bytes)
        manifest["composition_lock"].update(sha256=digest(lock_bytes), size_bytes=len(lock_bytes))
        for record in manifest["abis"]:
            record["shared_plan"] = "OFF"
        path.write_text(json.dumps(manifest), encoding="utf-8")
        self.assertEqual(inspector.inspect(path)["result"], "PASS")
        manifest["abis"][0]["shared_plan"] = "ON"
        path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "ABI shared plan build profile mismatch"):
            inspector.inspect(path)

    def test_observation_profile_mismatch_is_rejected(self):
        root, path, manifest, apk, entries = fixture("observation-mismatch")
        lock = json.loads((root / "composition-lock.json").read_bytes())
        lock["toolchain"] = {"job_observation": "ON"}
        lock_bytes = json.dumps(lock).encode()
        (root / "composition-lock.json").write_bytes(lock_bytes)
        manifest["composition_lock"].update(sha256=digest(lock_bytes), size_bytes=len(lock_bytes))
        path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "ABI job observation build profile mismatch"):
            inspector.inspect(path, apk)
        for item in manifest["abis"]:
            item.update(job_observation="ON", rife_build_fingerprint="d" * 64)
        path.write_text(json.dumps(manifest), encoding="utf-8")
        updated = []
        for name, data in entries:
            if name.endswith("/receipt.json"):
                receipt = json.loads(data)
                receipt["composition_lock_sha256"] = digest(lock_bytes)
                receipt["job_observation"] = "OFF"
                data = json.dumps(receipt).encode()
            updated.append((name, data))
        apk.write_bytes(zipped(updated))
        with self.assertRaisesRegex(ValueError, "receipt differs"):
            inspector.inspect(path, apk)
        for index, (name, data) in enumerate(updated):
            if name.endswith("/receipt.json"):
                receipt = json.loads(data)
                receipt["job_observation"] = "ON"
                updated[index] = (name, json.dumps(receipt).encode())
        apk.write_bytes(zipped(updated))
        self.assertEqual(inspector.inspect(path, apk)["result"], "PASS")

    def test_openmp_notice_required_in_each_distribution(self):
        for missing in (None, "jni_aar", "sample_apk", "native_payload_archive", "bil_apk", "lock_mismatch"):
            root, path, manifest, apk, apk_entries = fixture("openmp-notice-" + str(missing))
            lock = json.loads((root / "composition-lock.json").read_bytes())
            lock["toolchain"] = {"android_static_openmp": "ON"}
            lock_bytes = json.dumps(lock).encode()
            (root / "composition-lock.json").write_bytes(lock_bytes)
            manifest["composition_lock"].update(sha256=digest(lock_bytes), size_bytes=len(lock_bytes))
            notice_data = b"fixture LLVM notice"
            notice_path = root / "notices/llvm-openmp-NOTICE.txt"
            notice_path.parent.mkdir()
            notice_path.write_bytes(notice_data)
            manifest["notices"].append({"path": "notices/llvm-openmp-NOTICE.txt", "sha256": digest(notice_data), "size_bytes": len(notice_data)})
            lock["toolchain"]["openmp_notice"] = dict(manifest["notices"][-1])
            if missing == "lock_mismatch":
                lock["toolchain"]["openmp_notice"]["sha256"] = "a" * 64
            lock_bytes = json.dumps(lock).encode()
            (root / "composition-lock.json").write_bytes(lock_bytes)
            manifest["composition_lock"].update(sha256=digest(lock_bytes), size_bytes=len(lock_bytes))
            for key in ("jni_aar", "sample_apk", "native_payload_archive"):
                original = root / manifest[key]["path"]
                with zipfile.ZipFile(original) as source:
                    content = [(name, source.read(name)) for name in source.namelist()]
                if key != missing:
                    name = "META-INF/llvm-openmp-NOTICE.txt" if key == "native_payload_archive" else "assets/licenses/llvm-openmp-NOTICE.txt"
                    content.append((name, notice_data))
                rebuilt = zipped(content)
                original.write_bytes(rebuilt)
                manifest[key].update(sha256=digest(rebuilt), size_bytes=len(rebuilt))
            rebuilt_apk = []
            for name, value in apk_entries:
                if name.endswith("/receipt.json"):
                    receipt = json.loads(value)
                    receipt["composition_lock_sha256"] = digest(lock_bytes)
                    receipt["openmp_notice_sha256"] = digest(notice_data)
                    value = json.dumps(receipt).encode()
                rebuilt_apk.append((name, value))
            if missing != "bil_apk":
                rebuilt_apk.append(("assets/licenses/frame-interpolation/llvm-openmp-NOTICE.txt", notice_data))
            apk.write_bytes(zipped(rebuilt_apk))
            path.write_text(json.dumps(manifest), encoding="utf-8")
            if missing:
                expected = "OpenMP notice differs from composition lock" if missing == "lock_mismatch" else "OpenMP notice mismatch"
                with self.assertRaisesRegex(ValueError, expected):
                    inspector.inspect(path, apk)
            else:
                self.assertEqual(inspector.inspect(path, apk)["result"], "PASS")

    def test_complete_package_and_explicit_scope(self):
        _, manifest, _, apk, _ = fixture("ok")
        result = inspector.inspect(manifest, apk)
        self.assertEqual(result["result"], "PASS")
        self.assertEqual(result["device_qualification"], "NOT_EVALUATED")
        self.assertEqual(result["release_qualification"], "NOT_EVALUATED")
        self.assertEqual(inspector.inspect(manifest)["bil_apk"], "NOT_PROVIDED")

    def test_missing_abi_is_rejected(self):
        _, path, manifest, apk, _ = fixture("missing-abi")
        manifest["abis"].pop()
        path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "both distinct"):
            inspector.inspect(path, apk)

    def test_changed_native_and_bad_receipt_are_rejected(self):
        root, path, manifest, apk, entries = fixture("changed")
        (root / manifest["abis"][0]["path"]).write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "hash mismatch|size mismatch"):
            inspector.inspect(path, apk)
        root, path, manifest, apk, entries = fixture("bad-receipt")
        key, data = entries[1]
        receipt = json.loads(data)
        receipt["composition_lock_sha256"] = "d" * 64
        entries[1] = (key, json.dumps(receipt).encode())
        apk.write_bytes(zipped(entries))
        with self.assertRaisesRegex(ValueError, "receipt differs"):
            inspector.inspect(path, apk)

    def test_duplicate_zip_entry_and_native_owner_rejected(self):
        _, path, _, apk, entries = fixture("duplicate")
        apk.write_bytes(zipped(entries + [entries[0]]))
        with self.assertRaisesRegex(ValueError, "duplicate ZIP"):
            inspector.inspect(path, apk)
        _, path, _, apk, entries = fixture("two-owners")
        apk.write_bytes(zipped(entries + [("lib/arm64-v8a/" + inspector.LIBRARY, b"extra")]))
        with self.assertRaisesRegex(ValueError, "conflicting native owner"):
            inspector.inspect(path, apk)
        for i, basename in enumerate(("libinnocent_frame_interpolation.so", "libncnn.so", "libmeguviz_media_identity.so")):
            _, path, _, apk, entries = fixture("conflicting-owner-" + str(i))
            apk.write_bytes(zipped(entries + [("lib/arm64-v8a/" + basename, b"extra")]))
            with self.assertRaisesRegex(ValueError, "conflicting native owner"):
                inspector.inspect(path, apk)
        _, path, _, apk, entries = fixture("unrelated-native")
        apk.write_bytes(zipped(entries + [("lib/arm64-v8a/libdanmaku_advanced.so", b"unrelated")]))
        self.assertEqual(inspector.inspect(path, apk)["result"], "PASS")
        _, path, _, apk, entries = fixture("unknown-loader-abi")
        apk.write_bytes(zipped(entries + [("assets/native-packing/x86/VisionTool.zip", b"extra")]))
        with self.assertRaisesRegex(ValueError, "loader assets or ABI set"):
            inspector.inspect(path, apk)

    def test_missing_material_and_manifest_path_escape_rejected(self):
        _, path, manifest, apk, _ = fixture("missing-material")
        manifest.pop("jni_aar")
        path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "invalid material descriptor"):
            inspector.inspect(path, apk)
        root, path, manifest, apk, _ = fixture("escape")
        external = BASE / "external.aar"
        external.write_bytes(b"outside")
        manifest["jvm_api"] = {"path": "../external.aar", "sha256": digest(b"outside"), "size_bytes": 7}
        path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "outside composition"):
            inspector.inspect(path, apk)

    def test_invalid_jar_and_nested_native_rejected(self):
        for i, jar in enumerate((b"not-a-jar", zipped([(inspector.JNI_CLASS_ENTRY, CLASS_BYTES),
                                                        ("lib/arm64-v8a/libncnn.so", b"hidden")]))):
            root, path, manifest, apk, _ = fixture("bad-api-jar-" + str(i))
            aar = zipped([("AndroidManifest.xml", b"xml"), ("classes.jar", jar)])
            (root / "api.aar").write_bytes(aar)
            manifest["jvm_api"].update(sha256=digest(aar), size_bytes=len(aar))
            path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaises((ValueError, zipfile.BadZipFile)):
                inspector.inspect(path, apk)
        root, path, manifest, apk, _ = fixture("missing-jni-class")
        aar = zipped([("AndroidManifest.xml", b"xml"), ("classes.jar", zipped([("META-INF/MANIFEST.MF", b"ok")]))])
        (root / "api.aar").write_bytes(aar)
        manifest["jvm_api"].update(sha256=digest(aar), size_bytes=len(aar))
        path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "missing MediaNative class"):
            inspector.inspect(path, apk)
        root, path, manifest, apk, _ = fixture("native-in-extra-jar")
        good_jar = zipped([("META-INF/MANIFEST.MF", b"ok"), (inspector.JNI_CLASS_ENTRY, CLASS_BYTES)])
        bad_jar = zipped([("nested/libncnn.so", b"hidden")])
        aar = zipped([("AndroidManifest.xml", b"xml"), ("classes.jar", good_jar), ("libs/extra.jar", bad_jar)])
        (root / "api.aar").write_bytes(aar)
        manifest["jvm_api"].update(sha256=digest(aar), size_bytes=len(aar))
        path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "nested native library"):
            inspector.inspect(path, apk)
        root, path, manifest, apk, _ = fixture("corrupt-jar-entry")
        jar_bytes = io.BytesIO()
        with zipfile.ZipFile(jar_bytes, "w", compression=zipfile.ZIP_STORED) as archive:
            archive.writestr(inspector.JNI_CLASS_ENTRY, CLASS_BYTES)
        damaged = bytearray(jar_bytes.getvalue())
        index = damaged.index(CLASS_BYTES)
        damaged[index + 12] ^= 1
        aar = zipped([("AndroidManifest.xml", b"xml"), ("classes.jar", bytes(damaged))])
        (root / "api.aar").write_bytes(aar)
        manifest["jvm_api"].update(sha256=digest(aar), size_bytes=len(aar))
        path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "corrupt entry"):
            inspector.inspect(path, apk)


class SourceLockTests(unittest.TestCase):
    def test_exact_snapshot_and_changed_or_tampered_source(self):
        root = BASE / "source-lock"
        root.mkdir()
        sources = {}
        for project in inspector.PROJECTS:
            (root / project).mkdir()
            files = [{"path": "source.cpp", "sha256": "a" * 64}]
            sources[project] = {"revision": "b" * 40, "source_tree_sha256": inspector.canonical_hash(files), "files": files}
        lock = {"schema_version": 1, "sources": sources, "shader_inputs": [],
                "shaderpack_fingerprint": inspector.canonical_hash([]), "runtime_compiler_retained": True, "toolchain": {}}
        path = root / "lock.json"
        path.write_text(json.dumps(lock), encoding="utf-8")
        def selected(project): return sources[Path(project).name]
        with patch.object(inspector, "source_identity", side_effect=selected):
            result = inspector.verify_development_lock(path, root)
            self.assertEqual(result["scope"], "DEVELOPMENT_PROJECT_SOURCE_LOCK_ONLY")
            self.assertEqual(result["dependency_source_lock"], "NOT_INCLUDED")
            def changed(project):
                value = selected(project).copy()
                if Path(project).name == "MeguvizRIFE": value["revision"] = "c" * 40
                return value
            with patch.object(inspector, "source_identity", side_effect=changed):
                with self.assertRaisesRegex(ValueError, "live source differs"):
                    inspector.verify_development_lock(path, root)
            lock["sources"]["MeguvizRIFE"]["source_tree_sha256"] = "d" * 64
            path.write_text(json.dumps(lock), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "invalid locked source tree digest"):
                inspector.verify_development_lock(path, root)

    def test_build_refuses_output_inside_project_before_reading_inputs(self):
        root = BASE / "build-preflight"
        root.mkdir()
        for project in inspector.PROJECTS: (root / project).mkdir()
        with self.assertRaisesRegex(ValueError, "outside source projects"):
            inspector.build_candidate(root / "missing-lock.json", root, root / "missing-api.aar", "a" * 64,
                                      root / "MeguvizMedia" / "new-output")
        self.assertFalse((root / "MeguvizMedia" / "new-output").exists())
        (root / "dependencies").mkdir()
        with self.assertRaisesRegex(ValueError, "outside source projects and dependencies"):
            inspector.build_candidate(root / "missing-lock.json", root, root / "missing-api.aar", "a" * 64,
                                      root / "dependencies" / "new-output")
        self.assertFalse((root / "dependencies" / "new-output").exists())


if __name__ == "__main__":
    print("fixture_root=" + str(BASE), flush=True)
    unittest.main()
