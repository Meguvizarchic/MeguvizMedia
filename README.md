<div align="center">

<img src="docs/brand/MeguvizMedia.svg" width="96" height="96" alt="MeguvizMedia">

# MeguvizMedia

**C++17 帧 / 时间线处理与 RIFE effect 库**

C ABI · identity / RIFE · Android AHB 管线

<br>

[![License](https://img.shields.io/badge/license-GPL--3.0-00AEEC?style=flat-square&labelColor=2d3a55)](LICENSE)
[![Language](https://img.shields.io/badge/lang-C%2B%2B-DEA584?style=flat-square&labelColor=2d3a55)](https://github.com/Meguvizarchic/MeguvizMedia)
[![Issues](https://img.shields.io/github/issues/Meguvizarchic/MeguvizMedia?style=flat-square&labelColor=2d3a55)](https://github.com/Meguvizarchic/MeguvizMedia/issues)

</div>

---

当前提供 C++17 帧/时间线 core、公开 C ABI、identity/RIFE effect、独立调度准入，以及 Android OES→自有全分辨率 RGBA8 AHB→Compute/RIFE→AHB→GLES 的功能候选。Android 组合以 `libmeguviz_media_rife_ncnn.so` 作为目标 native owner，双 ABI 都保留原 Vulkan NN 的 ByteBuffer 兼容路径。真实 GPU、图像等价、显示与 A/V 资格尚未通过，详见 `STATUS.json`。

`src/core` 不依赖 RIFE、Android 或 bil。可选 `MeguvizMedia::rife_integration` 只消费 RIFE 公共 API；`MeguvizMedia::android` 消费 Compute 的 AHB/layout/FOREIGN/fence 合同。HostBridge 接口把播放器时钟、宿主恢复、音频和反射留给宿主，通用 SDK 不含 bil hook。

已运行验证：

- C ABI 边界：先读取 8 字节首部，再复制调用方声明的前缀；旧描述符可省略尾部保留字段，短于最小前缀、未知必需 flag/major 与非零保留字段明确拒绝。真实页边界回归覆盖输入和输出，详见 `artifacts/M1/descriptor-compatibility/summary.json`。
- 纯 C identity consumer：真实资源、一次异步回调、拒绝不转移引用、输出跨 session/domain 外部引用存活。
- 时间线：epoch、旧快照、倍速变化、整数时间换算与溢出；直接运行原 Kotlin oracle，与 C++ normal/delayed 输出逐行对照。
- SDK 安装后在仓库外独立 CMake/C consumer 构建运行。
- 可选 RIFE adapter：模型大小/hash、端点逐字节、alpha0.5 真推理与输出时刻。
- host 组合：从安装 prefix 链接一个 DSO，实际导出与批准名单相等，依赖无第二份 Meguviz/ncnn DSO。
- arm64-v8a 与 armeabi-v7a Android API27 CPU/reference SDK 交叉构建；API-only/JNI AAR 与独立处理 APK 构建。
- 包检查：JVM 类无静态加载器、API-only 已检查条目无 native、各 ABI 的已知 native owner 包路径、AAR/payload 同字节、ELF 与 APK 16KB 对齐。

Windows 本机复现入口：

```powershell
& 'C:\Users\Administrator\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' tools/run_host.py
& 'C:\Users\Administrator\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' tools/run_rife.py
```

`run_rife.py` 消费 Compute 与 RIFE 的安装 prefix，不自行获取另一份 ncnn。脚本读取合法本地研究模型，模型权重不会进入源码、SDK、AAR、payload 或 APK。

当前纯 core identity SDK 使用 `tools/build_android_identity.py` 生成 M6 独立安装前缀（包含 planner/cadence/vsync JNI，不含 RIFE/ncnn，也不覆盖 M1 历史证据），然后在 `android` 运行 wrapper：

```powershell
.\gradlew.bat --offline --no-daemon :media-api:assembleRelease :media-jni:assembleRelease :sample-processing:assembleDebug
```

普通 identity SDK 使用 JNI AAR；按需加载消费者使用 API-only AAR 与同 release AAR 导出的 native payload，不能在同一个应用同时引入两种 native owner。`MediaRuntime` 只有显式加载或 attach 方法，没有静态 `loadLibrary`。

官方组合候选的可复建入口：

```text
android/gradlew.bat :media-api:assembleRelease
python tools/build_android_composition.py --abi all --jvm-api android/media-api/build/outputs/aar/media-api-release.aar
python tools/package_android_composition.py
```

首次构建时 `:media-api:assembleRelease` 可在没有组合清单的工作区运行。`package_android_composition.py` 会显式传入组合目录。直接运行 Gradle 组合任务时，请提供 `-Pmeguviz.compositionDir=<含 manifest.json 的组合目录>`。清单或锁无效时，该打包构建会失败。

正式组合会编译可选的共享 GPU 计划协议，宿主需显式开启策略才会使用。如需构建省略该能力的组合，请传 `--shared-plan off`（CMake 对应 `-DMGM_ANDROID_SHARED_PLAN=OFF`）。每个 ABI 的构建回执和组合锁都会记录此编译选项。

官方 Android 组合默认使用锁定 NDK 的静态 OpenMP；仅 CPU/宿主和独立 Compute 默认不变。需要旧 OFF 构建时可追加 `--openmp off`（CMake 对应 `-DMGM_ANDROID_STATIC_OPENMP=OFF`），生成独立组合及不同私有指纹。ON 构建将 NDK LLVM 通知纳入组合清单和分发材料；两种构建仍须分别完成对应设备与质量验证。

`artifacts/M6/android-composition/manifest.json` 记录 canonical strip 后的每 ABI hash/size/private SDK、组合锁、API AAR 和 shader source/schema 指纹。`finalized=true` 仅代表二进制封包完成。普通应用消费 `media-rife-jni`；按需 loader 消费 native-payload 和不含 native 的 media-api，二选一。JVM 显式 `MediaRuntime.loadPackaged()`、`loadValidatedAbsolutePath()` 或 `attachAlreadyLoaded()`，无静态加载。

只读核对组合包及可选的 bil loader APK：

```text
python tools/mgv_integrate.py inspect-package --manifest <composition-manifest.json> --bil-apk <bil.apk> --out <new-report.json>
```

Windows 可使用 `tools/mgv-integrate.cmd`；若 Python 不在 PATH，先将 `MGV_PYTHON` 设为 Python 3 可执行文件路径。省略 `--bil-apk` 时报告明确标记 bil 包未检查；`--out` 不覆盖既有文件。此入口核对 manifest 锁哈希、两 ABI、API/JNI AAR、native payload、样例 APK、许可材料与提供的 bil loader/receipt 身份，并检查已知冲突库名。该包结构检查不能发现改名的 DSO 或静态嵌入的库，不能单独证明运行时 owner 唯一；任意更深层嵌套归档、Java 字节码语义、ELF、设备行为和发布资格也未评估。

开发源码组合锁可独立检查；与当前源码不一致时拒绝构建，正式发布锁与尚未填写的模板不冒充已受支持的格式：

```text
python tools/mgv_integrate.py verify-lock --lock <composition-lock.json> --source-root <workspace> --out <new-report.json>
python tools/mgv_integrate.py build --profile bil-native-payload --lock <composition-lock.json> --source-root <workspace> --jvm-api <api.aar> --jvm-api-sha256 <64-hex-sha256> --out <new-build-directory>
```

隔离构建要求输出目录不存在，且位于工作区内但不在四项目源码目录中。脚本将双 ABI 构建和日志保存在新目录，检查源码、着色器、外部依赖和工具的构建前后身份，并记录 payload 哈希；API AAR 由调用方提供并按其显式哈希校验。现有开发锁未完整绑定外部依赖与工具二进制，观察到的身份不是正式发布锁；单次成功也不证明跨目录二进制可复现。该构建不生成 JNI AAR、不完成设备或发布资格；完整分发包仍由组合打包流程生成。

`AndroidMediaSession` 在专用 GL owner 上 capture/draw，异步 submit 使用 effect worker、最多一份 accepted 推理及三份完成结果。pool slot 由 Compute 的最后内容 lease callback 解锁；GL sampling fence 保活输出。旧 epoch、deadline、cancel、drain/close 的 BUSY 都保留物理退役规则。HOST_WAIT 与 native-fence 路径显式选择，能力不满足时拒绝，由宿主选择回退。方向/四点降采样保持旧 OES 捕获语义，模型原图 history 不缩成 reduced tensor。

`sample-player` 是无 bil/Xposed 的 MediaExtractor/MediaCodec→OES→AHB identity 视频样例（无音频），未知 SDR 元数据或不合格能力保留 OES 显示。`sample-processing` 保留早期 CPU identity 图像样例。没有真机时只报告构建/封包；实际 AHB/GPU 同步、颜色/guard 等价、长播、真实显示和 A/V 仍需设备证据。decoder AImageReader、SurfaceControl、HDR 不加入默认路径。

ABI 与生命周期权威见 `include/meguviz` 和工作区 `contracts`。来源、许可与当前边界见 `NOTICE.md`、`docs/adr`；所有实际命令、输出与哈希位于 `artifacts`。
