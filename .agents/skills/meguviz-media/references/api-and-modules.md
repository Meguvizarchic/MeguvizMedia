# MeguvizMedia API 与模块速查

本文档整理 MeguvizMedia `1.2.2` 的 C/C++ 目标、Android 模块与 3 种装配方案、构建工具脚本、以及公开 C/Java 接口签名。

---

## 1. C/C++ 目标与构建选项（`CMakeLists.txt`）

### 消费方接入

```cmake
find_package(MeguvizMedia 1.2.2 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE MeguvizMedia::core)
```

- **`MeguvizMedia::core`**：静态库 `meguviz_media_core`，包含帧元数据、时钟映射、时间线规划、调度控制与节拍统计，传递依赖 `MeguvizCompute::core` 与 `Threads::Threads`。
- **`MeguvizMedia::android`**：仅 Android 交叉编译可用，含 `src/android/ahb_bridge.cpp`，链接 `android`、`EGL`、`GLESv3`。
- **`MeguvizMedia::rife_integration`**：开启 `-DMGM_WITH_RIFE=ON` 时可用，适配 `MeguvizRIFE::core`（`<meguviz/media_rife.h>`）。
- **演示/回放工具**：`mgm_trace_replay`（`examples/headless_timeline.cpp`）。

| CMake 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `MGM_BUILD_TESTS` | `ON` | 构建 C ABI、时间线、调度控制及内存池契约测试 |
| `MGM_WITH_RIFE` | `OFF` | 启用可选的 RIFE SPI 适配器 `MeguvizMedia::rife_integration` |
| `MGM_COMPUTE_SOURCE` | `""` | 开发期源码路径（未设置时通过 `find_package(MeguvizCompute CONFIG REQUIRED)` 查找） |
| `MGR_ENABLE_JOB_OBSERVATION` | `OFF` | 开启后允许透传逐任务观测快照 `mgm_rife_frame_observation` |

工具链基线：CMake `3.31+`、C++17、MSVC 静态运行库 `MultiThreaded$<$<CONFIG:Debug>:Debug>`；Android NDK `30.0.16248370`、CMake `4.1.2`、API `27+`、`c++_static`。

---

## 2. Android 模块、装配模式与工具链

Android 模块统一使用 **JDK 17**、**Android SDK 37**、`minSdk 27`，版本号 `1.2.2`。

### 三种装配方案（每个消费方只选一种）

| 模块 / 方案 | Gradle / 产物形态 | 原生加载入口 (`MediaRuntime`) | 适用对象与约束 |
| --- | --- | --- | --- |
| **Identity AAR** (`media-jni`) | `io.github.jichuo1.meguviz.media.jni`，内含每 ABI `libmeguviz_media_identity.so` | `loadPackagedIdentity()` | 独立 CPU/reference 传递与时间线验证；不含 RIFE 或 ncnn |
| **官方组合 AAR** (`media-rife-jni`) | `io.github.jichuo1.meguviz.media.rife.jni`，内含 `libmeguviz_media_rife_ncnn.so` | `loadPackaged()` | 常规 Android 播放器应用；Compute + Media + RIFE + ncnn 单一静态闭包 |
| **按需加载组合** (`media-api` + `native-payload`) | `io.github.jichuo1.meguviz.media.api`（纯 Java）+ 同批产物 `libmeguviz_media_rife_ncnn.so` | `loadValidatedAbsolutePath(path)` 或 `attachAlreadyLoaded()` | 拥有自研远程/热下载加载器的宿主；严禁再引入 `media-rife-jni` |

### 可选 QCOM 运动估计后端

- 产物：`libmeguviz_qcom_motion.so`（目前校验固定字节版针对 `arm64-v8a`，Android API 27+，启用 16 KB 页对齐 `-Wl,-z,max-page-size=16384`）。
- 构建脚本：`python src/android/qcom_motion/build_so.py --ndk-bin <NDK_LLVM_BIN> --out <新输出目录>`。
- 装配：直接按 ABI 放入宿主 `jniLibs/arm64-v8a/`，不修改 RIFE 组合 manifest 与既有原生布局。

### 组合构建与校验脚本（`tools/`）

- `tools/build_android_composition.py`：生成官方单一静态闭包 `libmeguviz_media_rife_ncnn.so`（支持 `--abi`、`--jvm-api`、`--ncnn-layer-profile [full|rife46|rife46-arm64]`、`--packed-relocations [on|off]`、`--openmp [on|off]`、`--shared-plan [on|off]`、`--job-observation [on|off]`）。
- `tools/build_android.py`：交叉构建用于 `media-jni` 的纯 CPU/Identity Media SDK（`--ndk`、`--cmake`、`--ninja`）。
- `tools/mgv_integrate.py`（Windows 可调用 `tools/mgv-integrate.cmd`）：
  - `inspect-package --manifest <manifest.json> [--bil-apk <apk>] [--out <report.json>]`：检验组合包与宿主包内原生库命名及配置一致性。
  - `verify-lock --lock <lock.json> [--source-root <root>]`：校验四仓源码锁。
  - `build --profile bil-native-payload --lock <lock.json> --jvm-api <aar> --jvm-api-sha256 <hex> --out <dir>`：隔离构建按需载荷包。

---

## 3. 公开 C ABI 函数速查（`include/meguviz/*.h`）

版本：`MGM_SPI_MAJOR = 0u`、`MGM_SPI_MINOR = 1u`。所有结构体须清零并初始化 `MGC_HEADER_INIT(Type)`。

- **帧、时钟与 Identity（`media.h`）**：
  - `mgm_frame_create` / `mgm_frame_retain` / `mgm_frame_release` / `mgm_frame_describe`
  - `mgm_timeline_create` / `mgm_timeline_release` / `mgm_timeline_reset` / `mgm_timeline_may_present`
  - `mgm_identity_effect_create` / `mgm_identity_effect_release`
- **时间线规划器（`timeline.h`）**：
  - `mgm_planner_create` / `mgm_planner_release` / `mgm_planner_begin` / `mgm_planner_begin_target` / `mgm_planner_close_epoch`
  - `mgm_planner_effective_delay` / `mgm_planner_counters` / `mgm_planner_clock`
  - `mgm_planner_offer` / `mgm_planner_plan` / `mgm_planner_valid` / `mgm_planner_commit` / `mgm_ticks_to_ns`
- **节拍、Vsync 与有界调度（`media_cadence.h`、`media_scheduler.h`）**：
  - `mgm_cadence_create` / `mgm_cadence_release` / `mgm_cadence_add` / `mgm_cadence_reset` / `mgm_cadence_snapshot`
  - `mgm_vsync_grid_create` / `mgm_vsync_grid_release` / `mgm_vsync_grid_on_vsync` / `mgm_vsync_grid_snapshot`
  - `mgm_vsync_meter_create` / `mgm_vsync_meter_release` / `mgm_vsync_meter_add`
  - `mgm_admission_create` / `mgm_admission_release` / `mgm_admission_reserve` / `mgm_admission_retire`
- **Android 会话与阶段建池（`media_android.h`）**：
  - 预热与创建服务：`mgm_android_creation_prepare` / `mgm_android_creation_reserve` / `mgm_android_creation_release_reservation` / `mgm_android_creation_cancel_import` / `mgm_android_creation_service_owner` / `mgm_android_creation_service_background` / `mgm_android_creation_discard_ready`
  - 同步与分阶段创建：`mgm_android_session_create` / `mgm_android_session_create_reserved` / `mgm_android_session_begin_reserved` / `mgm_android_creation_import_background` / `mgm_android_session_commit_initialization`
  - 会话执行：`mgm_android_probe` / `mgm_android_capture_oes` / `mgm_android_compare_inputs` / `mgm_android_draw` / `mgm_android_poll` / `mgm_android_defer_frame_release` / `mgm_android_service_retirement` / `mgm_android_begin_epoch` / `mgm_android_set_event_handler` / `mgm_android_session_close`
- **可选 RIFE 桥接（`media_rife.h`）**：
  - `mgm_rife_effect_create` / `mgm_rife_effect_release` / `mgm_rife_frame_observation`

---

## 4. 公开 Java API 速查（`io.github.jichuo1.meguviz.media`）

- **`MediaRuntime`**：
  - `loadPackagedIdentity()` / `loadPackaged()` / `loadValidatedAbsolutePath(String library)` / `attachAlreadyLoaded()` / `isAttached()`
  - `frameSetOptimizationFlags(long engine, int flags)` / `frameSetShaderpack(long engine, String rootDir, String canonicalManifestHash, String bindingsJson)` / `identityRgb(byte[] rgb, int width, int height, int stride)`
- **`FrameGenerationProfile`**：
  - 构造：`FrameGenerationProfile(width, height, [outputWidth, outputHeight,] primaries, matrix, sdr8Bit, sourcePeriodNs, displayPeriodNs, midpointBudgetNs)`
  - 登记：`qualify(Backend backend, long pairToDrawP95Ns)`、`qualifyRife([processingWidth, processingHeight,] syncPath, flowScale, pairToDrawP95Ns)`
  - 筛选：`selectRife(Qualification... variants)`（优先大像素数 -> 细 flow -> 低延时 -> `AHB_HOST_WAIT`）、`select(Qualification rife, Qualification qcom)`（两路均达标时优先 RIFE）
- **`FrameGenerationSession`**（实现 `WindowBackend`）：
  - 开启：`open(profile, rife, preparedRifeSupplier, flowScale, qcom)`、`openQualified(profile, rife, preparedRife, qcom)`
  - 生命周期与调度：`backend()`、`capture(oes, transform, sourceId, ptsUs)`、`lastCaptureStatus()`、`inputsReady(left, right)`、`captureTiming(frame)`、`trySubmit(left, right, deadlineNs)`（返回 `Submission`，含 `Decision`: `ACCEPTED` / `IN_FLIGHT` / `DEADLINE` / `INPUT_NOT_READY` / `BACKEND_REJECTED` 与 `jobId`）、`submit(left, right, deadlineNs)`、`invalidate()`、`cancel(id)`、`poll()`、`draw(frame, width, height)`、`release(frame)`、`close()`
- **`AndroidMediaSession`**：
  - 创建：`new AndroidMediaSession(...)`、`identity(...)`、异步分阶段 `newPreparation(...)` / `prepareAsync(...)`（含 `start()` / `beginOnGl()` / `commitOnGl()` / `cancel()` / `retirementStatus()`）及 `servicePendingCreation()`
  - 核心调用：`probe()`、`executionInfo()`、`jobObservation(frame)`、`beginEpoch(next)`、`setPaused(value)`、`capture(...)`、`compare(left, right)`、`interpolate(...)`、`submit(...)`、`pollCompleted()`、`cancel(jobId)`、`draw(frame, w, h)`、`poll(timeoutNs)`、`deviceLossStatus()`、`deviceState()`、`close()`、`Frame.close()`

---

## 5. 家族库协作边界

- **MeguvizRIFE（`1.2.2`）**：仅封装在 `media-rife-jni` 或 C++ `MeguvizMedia::rife_integration` 内部；不要在同一个 Android 进程里再另起第二份 `MeguvizRIFE` 动态库或 ncnn 实例。
- **MeguvizMemory（`1.2.2`）**：若仅做 GPU OES/AHB 补帧，完全不需要 Memory；仅当业务要把上游写入或由 Media/Compute 产出的完整 CPU `READ` 帧零拷贝交给 Memory 消费时，才从 Memory 仓库引入 `MeguvizMemoryMediaReadBridge::read_bridge`（`adapters/media`）。
