"""Link the AHB-capable QCOM motion JNI shared object.

Records the linker recipe for the shipped JNI library and pins its bytes so an
accidental toolchain or source drift is caught at build time.

Usage:
    python build_so.py --ndk-bin <ndk toolchain bin> --out <output directory>

Verified with NDK 30.0.16248370, clang --target=aarch64-linux-android27:
    libmeguviz_qcom_motion.so
    sha256 6022dad67c2ab7f14b96f0f74a3ef7081a09194a117cbf5ab4c72cce72890285
    bytes  1235864
from the four translation units beside this script (three-level estimator with
the parallel-reduction pipeline plus the adaptive fence-wait strategy both
enabled by default, AHB bridge, JNI bridge, candidate projection).
Library order matters: DT_NEEDED is emitted in command-line order, so
-lEGL -lGLESv3 -lvulkan -landroid is required.
"""

import argparse
import hashlib
import os
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
EXPECTED_SHA256 = "6022dad67c2ab7f14b96f0f74a3ef7081a09194a117cbf5ab4c72cce72890285"
EXPECTED_BYTES = 1235864

# All inputs live beside this script, so the file works both in this package and
# after patch 01 places it at src/android/qcom_motion/build_so.py.
SOURCES = [
    HERE / "qcom_motion.cpp",
    HERE / "ahb_jni.cpp",
    HERE / "qcom_motion_jni.cpp",
    HERE / "qcom_candidate_projection.cpp",
]

# Order is load-bearing (DT_NEEDED follows command-line order).
LIBRARIES = ["-lEGL", "-lGLESv3", "-lvulkan", "-landroid"]

COMMON = [
    "--target=aarch64-linux-android27",
    "-std=c++17",
    "-O2",
    "-Wall",
    "-Wextra",
    "-Wno-missing-field-initializers",
    "-Werror",
    "-fexceptions",
    "-static-libstdc++",
    "-DVK_USE_PLATFORM_ANDROID_KHR",
    "-fPIC",
    "-shared",
    "-Wl,--no-undefined",
    "-Wl,-z,max-page-size=16384",
    "-fno-fast-math",
    "-ffp-contract=off",
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ndk-bin", type=Path, required=True,
                        help="NDK toolchains/llvm/prebuilt/<host>/bin directory")
    parser.add_argument("--out", type=Path, required=True,
                        help="output directory; must not already contain the target")
    args = parser.parse_args()

    compiler = args.ndk_bin / "clang++.exe"
    if not compiler.exists():
        compiler = args.ndk_bin / "clang++"
    assert compiler.exists(), f"missing NDK driver: {compiler}"
    target = args.out / "libmeguviz_qcom_motion.so"
    if target.exists():
        raise SystemExit(f"refusing to overwrite {target}")
    args.out.mkdir(parents=True, exist_ok=True)
    for source in SOURCES:
        assert source.exists(), f"missing source: {source}"

    argv = [str(compiler), *COMMON,
            "-I", str(HERE),
            *[str(source) for source in SOURCES],
            *LIBRARIES,
            "-o", str(target)]
    environment = os.environ.copy()
    scratch = args.out / "compiler-tmp"
    scratch.mkdir(exist_ok=True)
    environment["TMP"] = str(scratch)
    environment["TEMP"] = str(scratch)
    result = subprocess.run(argv, capture_output=True, env=environment)
    (args.out / "link.log").write_bytes(result.stdout + result.stderr)
    if result.returncode:
        raise SystemExit(result.returncode)

    blob = target.read_bytes()
    digest = hashlib.sha256(blob).hexdigest()
    print(f"sha256 {digest}")
    print(f"bytes  {len(blob)}")
    assert len(blob) == EXPECTED_BYTES, f"size drift: {len(blob)}"
    assert digest == EXPECTED_SHA256, f"hash drift: {digest}"
    print("byte-exact reproduction of the pinned JNI object")


if __name__ == "__main__":
    main()
