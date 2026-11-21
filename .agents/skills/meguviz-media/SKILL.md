---
name: meguviz-media
description: 在 C/C++ 或 Android 播放器中接入 MeguvizMedia（1.2.2）时间线、帧调度、Identity 传递或统一帧生成会话，将独立的 RIFE/QCOM 路径迁移到 FrameGenerationSession 并保留原帧与音画时钟；当仅需原生离线双图或直接调用推理内核时，转交 MeguvizRIFE（$meguviz-rife）。
---

# MeguvizMedia 接入与迁移

MeguvizMedia（`1.2.2`）为 C/C++ 媒体处理与 Android 播放器提供时间线规划、帧调度、Identity 校验以及统一中点补帧接口，由宿主继续掌握音频、时钟与原帧呈现权。

## 第 1 阶段：盘点工程现状

修改代码前先读取消费方工程并输出简要评估：

1. **构建与加载基线**：读取 `CMakeLists.txt` 或 Android `build.gradle.kts`、`settings.gradle.kts`，核实 C/C++ 要求（CMake 3.31+、C++17+、匹配的 `MeguvizCompute` 1.2.2 与 CRT），或 Android 要求（JDK 17、Android SDK 37、`minSdk 27`、NDK `30.0.16248370`、`c++_static`），确认进程内只有一个 Compute/RIFE/ncnn 原生库所有者。
2. **宿主播放与渲染链**：定位 OES/`SurfaceTexture` 视频输出、EGL 上下文所在线程（EGL owner）、音画时钟基准、Seek 与切清晰度入口，以及原帧绘制和保底回退路径。
3. **能力与边界核对**：`FrameGenerationSession` 仅适用于 **8 位 SDR** 且**显示周期短于源帧周期**的播放场景，固定在相邻两帧之间生成中点（`0.5`）插值帧，且**同一时间最多处理一个在途请求**（QCOM 另要求 BT.709 色彩空间）。若宿主需要任意 `alpha`、独立帧比较或并发多个在途任务，先保留原生 `AndroidMediaSession` 或既有路径，仅迁移已覆盖场景。

## 第 2 阶段：选择模块与接入依赖

每种消费方只选择一种装配方式，不要同时混装多个原生加载入口或引入无用的可选桥：

| 消费方与需求 | 接入方式与模块 | 边界说明 |
| --- | --- | --- |
| C/C++ 时间线、节拍与 Identity | `find_package(MeguvizMedia 1.2.2 CONFIG REQUIRED)`，链接 `MeguvizMedia::core` | 纯 C ABI（`<meguviz/media.h>`、`<meguviz/timeline.h>`、`<meguviz/media_cadence.h>`、`<meguviz/media_scheduler.h>`），不含 RIFE 或 Android 头文件 |
| C/C++ 集成 RIFE 推理 | 开启 `-DMGM_WITH_RIFE=ON`，链接 `MeguvizMedia::rife_integration` | 提供 `<meguviz/media_rife.h>`，传递依赖 `MeguvizRIFE::core` |
| CPU 帧对接 MeguvizMemory | 引入 Memory 源码 `adapters/media` 的 `MeguvizMemoryMediaReadBridge::read_bridge` | 仅当需要把完整且就绪的 CPU `READ` 帧交给 Memory 时引入，需匹配 SDK 与 CRT；GPU 补帧继续走 AHB 与 fence |
| Android Identity 验证（纯传递） | 引入含 Java API 的 `media-jni` AAR，调用 `MediaRuntime.loadPackagedIdentity()` | 含 `libmeguviz_media_identity.so`，仅用于传递与时间线验证，不含 RIFE |
| Android 播放器补帧（常规应用） | 引入含 Java API 的 `media-rife-jni` AAR，调用 `MediaRuntime.loadPackaged()` | 打包单一静态闭包 `libmeguviz_media_rife_ncnn.so` |
| Android 播放器补帧（按需加载） | 引入无原生库的 `media-api` AAR + 同批构建的 `native-payload`，调用 `MediaRuntime.loadValidatedAbsolutePath()` 或 `attachAlreadyLoaded()` | 与 `media-rife-jni` 互斥；不可与普通 JNI AAR 混用 |
| 可选 Qualcomm 运动估计后端 | 独立构建 `libmeguviz_qcom_motion.so` 并按 ABI（如 `arm64-v8a`）放到 `jniLibs` | 独立于 RIFE 组合 manifest，未打包或预检失败时自动跳过 QCOM |

## 第 3 阶段：执行最小迁移与生命周期对齐

### 播放器补帧接入骨架（Android `FrameGenerationSession`）

```java
// 1. 在 EGL owner 上初始化 Profile 与实测登记（热循环外）
FrameGenerationProfile profile = new FrameGenerationProfile(
    videoW, videoH, outW, outH, primaries, matrix, isSdr8Bit,
    sourcePeriodNs, displayPeriodNs, midpointBudgetNs);

// 若启用 RIFE：先在 EGL owner 上调用 preparedRife.probe() 并通过画质/预算检查
FrameGenerationProfile.Qualification rifeQual =
    profile.qualifyRife(procW, procH, AndroidMediaSession.AHB_HOST_WAIT, 4, measuredRifeP95Ns);
FrameGenerationProfile.Qualification qcomQual =
    profile.qualify(FrameGenerationProfile.Backend.QCOM_AHB, measuredQcomP95Ns);

FrameGenerationProfile.Qualification chosenRife = profile.selectRife(rifeQual);
FrameGenerationSession session = FrameGenerationSession.openQualified(
    profile, chosenRife, preparedRife, qcomQual);
// 关键所有权规则：仅当 session != null && session.backend() == Backend.RIFE 时才接管 preparedRife！
if (session == null || session.backend() != FrameGenerationProfile.Backend.RIFE) {
    preparedRife.close(); // 返回 QCOM_AHB 或 null 时，调用方仍需负责释放 RIFE 会话
}

// 2. 逐帧循环（全程在同一个 EGL owner 线程运行）
Object cur = session.capture(oesTex, rawTransform, nextSourceId++, ptsUs);
if (cur != null && prev != null) {
    long deadlineNs = System.nanoTime() + frameBudgetNs; // 必须采用 System.nanoTime() 时基
    FrameGenerationSession.Submission sub = session.trySubmit(prev, cur, deadlineNs);
    // sub.decision 为 IN_FLIGHT / DEADLINE / INPUT_NOT_READY / BACKEND_REJECTED 时直接呈还原帧
}
FrameGenerationSession.Completion done = session.poll();
if (done != null) {
    if (done.status == 0 && done.frame != null) {
        try { session.draw(done.frame, outW, outH); }
        catch (WindowBackend.Backpressure bp) { drawOriginalOes(); } // 遇背压无缝切回原帧
    }
    if (done.frame != null) session.release(done.frame);
}

// 3. Seek 时重置代际，退出前全部释放并关闭
session.invalidate(); // 旧帧仍需逐一 release()，在途任务仍需 poll() 至终态返回
// 退出播放或切换格式前：释放所有已持有的帧 -> 在 EGL owner 调用 session.close()
```

### 迁移与生命周期铁律

- **EGL owner 串行化**：`FrameGenerationSession` 与 `AndroidMediaSession` 的所有方法都必须在创建它的同一个 EGL owner 线程上调用（且窗口 EGL 上下文保持 current）。
- **严格递增输入与时基**：`capture()` 传入原始 `SurfaceTexture` 矩阵（`AndroidMediaSession` 内部会按规范完成垂直翻转，调用方勿额外叠加翻转）；提交给 `trySubmit()` 的左右帧必须属于同一代际且 `sourceId` 与 `ptsUs` 严格递增，`deadlineNs` 使用 `System.nanoTime()` 时基。
- **不中断原帧与音频时钟**：捕获失败、提交被拒、超过截止时间、非推理结果或 `draw` 抛出 `WindowBackend.Backpressure` 时，立刻无感呈现宿主原帧并保留音频与原播放时钟。
- **资源释放与延迟关闭**：`capture()` 与 `poll()` 返回的每一帧都必须且只能 `release()` 一次；调用 `close()` 前必须保证所有持有帧已归还（否则抛异常）。若 `close()` 因底层任务尚未完全结束返回繁忙或抛出回压异常，**保留 session、EGL owner 与上下文并稍后重试**，待后台任务结束并归还资源后再销毁 EGL 环境。

> **按需深入阅读**：完整 C/C++ 与 Java 接口签名、Gradle/CMake 构建及组合工具参数见 [API 与模块速查](references/api-and-modules.md)；迁移前后对照、异步准备取消与高频异常排障见 [迁移对照与排障](references/migration-patterns.md)。

## 第 4 阶段：验证与交付清单

1. **C/C++ 宿主构建与契约测试**（在 `MeguvizMedia` 根目录执行；若有同工具链的预安装 Compute SDK，可改传 `-DCMAKE_PREFIX_PATH`）：
   ```sh
   cmake --preset host-debug "-DMGM_COMPUTE_SOURCE=<Compute源码目录>" -DMGC_WITH_NCNN=OFF
   cmake --build --preset host-debug
   ctest --preset host-contracts --no-tests=error
   ```
2. **Android 模块测试与组合包校验**：
   ```sh
   android/gradlew.bat -p android :media-api:testDebugUnitTest :media-api:assembleRelease
   python tools/mgv_integrate.py inspect-package --manifest <manifest.json> --out <新报告.json>
   ```
3. **真实宿主端到端核对**：在真机播放器中覆盖连续播放、Seek 跳转、切清晰度、断流或超预算回退，用同一单调时基记录原帧/插帧实际呈现与音频同步，确认所有帧释放和 `close()` 成功退出。
4. **交付摘要**：明确报告选用的后端（RIFE / QCOM / Identity / 纯 C++ Core）、装配模式、保留的原帧回退条件及各项命令实测结果。
