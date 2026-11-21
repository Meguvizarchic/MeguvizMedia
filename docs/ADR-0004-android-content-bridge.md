# Android 内容桥与官方组合设计说明

本文说明 `MeguvizMedia::android` 的底层资源生命周期、JNI 官方单一静态闭包构建方式，以及 `FrameGenerationSession` 统一中点补帧入口的接入与资源归属规则。

## OES 内容桥与同期控制

`MeguvizMedia::android` 在保留宿主 OES 生产者的前提下，于同一专用共享 GL 上下文中按照调用方传入的 `SurfaceTexture`/裁剪/旋转/翻转变换矩阵绘制自有全分辨率 `RGBA8` `AHardwareBuffer`。未知色彩、HDR 或受保护内容不会进入此 8 位 SDR 接口。

- **输入与历史帧保活**：源生产者的内容租约须覆盖整个 `capture` 调用期间；Media 自有的历史帧槽位由 Compute 最终内容租约的回调固定，单纯持有 AHB 对象引用不代表可以安全覆写。
- **版本与池容量**：`allocation_id` 与 `allocation_generation` 在底层物理重建时更新，`content_id` 每次写入递增；内部池容量范围为 3–12 槽，池满时返回 `MGC_BACKPRESSURE`（`-8`）且不接管输入所有权。
- **同步模式**：
  - `AHB_HOST_WAIT`：使用 EGL fence 显式客户端等待，再由 Compute 执行 Vulkan acquire/release 与 `GENERAL` / `FOREIGN` 所有权交接。
  - `AHB_GPU_SYNC`：使用二进制 native `SYNC_FD`；GL 绘制结束后将采样完成 fence 登记为下一次 Compute acquire 依赖。若硬件或驱动不支持双向 fence，接口会直接返回拒绝状态，由宿主回退到已验证方案，绝不自动降级。

## 帧比较、调度与观测

- **稀疏采样比对**：`AndroidMediaSession.compare()` 通过稀疏 RGBA 网格读回检测重复帧、静止画面与切镜，同步插值路径也会执行该采样；统计性能开销时须连同读回字节数与等待耗时一并计入。RIFE 相关图像保护由 RIFE 内核执行，诊断信息随任务绑定到输出帧。
- **异步提交与有界排队**：`AndroidMediaSession` 在创建它的 EGL owner 线程上执行捕获、呈现和取帧。异步 `submit` 复用 RIFE 的工作线程，原生回调只向最多容纳 3 个结果的队列写入回执；调度器控制同一时间最多处理 1 个在途请求（`max_in_flight = 1`），默认基于最多 64 个成功完成（从接收到绘制就绪）的样本计算 p95 并叠加安全余量，失败或取消的任务不计入快速样本。
- **过期与关闭约束**：可按需关闭截止时间检查，但过期输出丢弃与浅队列限制始终保留。旧 `epoch` 的回调仅用于驱动底层清理而不允许呈现；`cancel()` 或 `close()` 不会在任务结束前强行释放输入或输出资源。请注意，GLES `draw` 与 `eglSwapBuffers` 只表示提交到绘制槽位，并不等于屏幕实际显示完成反馈。

## JNI 官方单一静态闭包构建

`MediaNative` 保留了原有的状态码、`ByteBuffer` 传参以及 `frameCreate`/`frameConfigure`/`frameLoad`/`frameProcess`/`frameClose` 接口语义，内部转调 Compute 与 RIFE 的公共 C API（统计 `LEGACY_CPU_BRIDGE` 时须把输入输出拷贝耗时算在内）。同一宿主进程中只能存在唯一一份 Compute/RIFE/ncnn 原生库。

`distribution/android` 将 `MeguvizCompute`、`MeguvizRIFE`、`MeguvizMedia`、固定版本的 `ncnn` 以及运行时 `glslang` 静态链接为单一闭包（仅链接一次 `c++_static`），只导出公共头文件中的 C API 与固定 JNI 类。`tools/build_android_composition.py` 为每个 ABI 生成规范化原生库并执行一次 `--strip-unneeded`，随后 `media-rife-jni` 直接消费这组未经二次裁剪的字节码；带有自研按需加载器的宿主则使用无原生库的 `media-api` AAR 搭配同批 `native-payload`（两种装配模式严禁混用）。清单中的 `finalized` 标记仅代表二进制构建与校验流程走完，不代表真机设备验收通过。

在 `MeguvizMedia` 仓库根目录执行以下命令即可构建 Android 组件（要求 JDK 17、Android SDK 37、`minSdk 27` 以及同级的 `MeguvizCompute`、`MeguvizRIFE` 和 `MeguvizShaderTools` 源码目录，可选 `--ndk`、`--cmake`、`--ninja` 指定自定义工具链路径）：

```text
android/gradlew.bat -p android :media-api:assembleRelease :media-api:testDebugUnitTest
python tools/build_android_composition.py --abi all --jvm-api android/media-api/build/outputs/aar/media-api-release.aar
android/gradlew.bat -p android -Pmeguviz.compositionDir=../artifacts/M6/android-composition :media-rife-jni:assembleRelease :sample-player:assembleDebug
```

- 默认使用 `full` 算子配置，并在 Android API 23 及以上启用 APS2 动态重定位压缩。
- 传入 `--ncnn-layer-profile rife46-arm64` 可仅在 `arm64-v8a` 上裁剪为 RIFE 4.6 所需算子、`armeabi-v7a` 保留全量；`--ncnn-layer-profile rife46` 用于双 ABI 裁剪对照；`--packed-relocations off` 用于关闭重定位压缩对照。若接入外部着色器包，须使用对应构建记录中的私有 SDK 指纹。
- 独立 `android/sample-player` 通过自身的 `MediaExtractor` / `MediaCodec` → OES 链路验证同一套 AHB Identity 传递（不含音频同步，未知或不兼容格式自动回退纯 OES 显示）。

## RIFE 与 QCOM 的统一 Android 入口

更多芯片与驱动参考见 [Android GPU 兼容性索引](ANDROID_GPU_COMPATIBILITY.md)。

`FrameGenerationSession` 在同一个持有当前 EGL 上下文的线程上串行完成 OES 采集、中点（`0.5`）双帧提交、轮询、绘制与释放：RIFE 复用准备好的 `AndroidMediaSession`；QCOM 复用紧凑 AHB 亮度缓冲与 GLES warp，并按输出尺寸映射纹理坐标、绘制到当前默认帧缓冲。窗口交换与后续状态恢复由宿主自行负责。

### 接入顺序

1. **构建 Profile 与登记实测指标**：根据源与输出分辨率、色彩空间、源帧周期、显示周期和单帧预算创建 `FrameGenerationProfile`（两路均要求 8 位 SDR 且显示周期短于源帧周期，QCOM 另要求 BT.709）。在目标设备上完成能力与画质检验后，通过 `qualify()` 或 `qualifyRife()` 登记实测的帧对到绘制提交 p95 耗时。格式、分辨率、帧率或设备发生变更时须重新创建会话与配置。
2. **在热循环外开启会话**：在 EGL owner 线程上调用 `open(...)` 或 `openQualified(...)`。两路均合格时优先选用 RIFE；若 QCOM 动态库缺失或预检失败会自动跳过；若返回 `null`，宿主直接按原帧路径播放。
3. **采集与入队调度**：在 EGL owner 线程更新 `SurfaceTexture` 后调用 `capture(oes, rawTransform, sourceId, ptsUs)`。`ptsUs` 为解码帧微秒时间戳，`sourceId` 与 `ptsUs` 须保持严格递增；`capture()` 内部已包含标准垂直翻转，调用方无需额外翻转矩阵。`trySubmit(left, right, deadlineNs)` 中的 `deadlineNs` 必须使用 `System.nanoTime()` 时基，返回的 `Submission.decision` 会明确区分 `IN_FLIGHT`、`DEADLINE`、`INPUT_NOT_READY` 与 `BACKEND_REJECTED`；一旦被拒绝或捕获失败，直接呈现宿主原帧。
4. **轮询、绘制与归还**：按宿主节拍调用 `poll()` 推进任务完成和后台资源回收（达到截止时间且仍未返回时，`poll()` 会发出一次取消请求，但槽位仍会持续占用直到任务结束）。收到可用输出帧时按音画时钟调度 `draw()`；遇空结果或抛出 `WindowBackend.Backpressure` 时立刻切回原帧。无论是 `capture()` 还是 `poll()` 产出的帧对象，用完后都必须在 EGL owner 上调用一次 `release()`。

```java
FrameGenerationSession.Submission submission = session.trySubmit(left, right, deadlineNs);
if (submission.jobId != 0) {
    pendingJobId = submission.jobId;
}
```

### Seek 重置、精确选型与受控回退

- **Seek 与关闭**：遇到 Seek 或时间戳跳变时调用 `invalidate()`，继续释放旧帧并持续 `poll()` 直至在途任务终态返回。切清晰度或退出播放时先停止提交并归还所有已持有的帧，再在同一个 EGL owner 上调用 `close()`；若因底层资源尚未回收而繁忙失败，**保留会话、owner 线程和 EGL 上下文**并在后续节拍重试，切勿强行销毁正在被 GPU 读取的资源。
- **选择已验证的 RIFE 配置**：使用 `qualifyRife(...)` 登记各候选处理尺寸、同步路径与 `flowScale`，经 `selectRife(...)` 过滤超预算项并按“最大处理像素数 → 更细 flow → 更低 p95 → `AHB_HOST_WAIT`”择优。提交给 `openQualified(...)` 的 `AndroidMediaSession` 必须先在 EGL owner 上完成一次 `probe()`（`AHB_GPU_SYNC` 要求双向 fence 均通过）；**仅当 `openQualified` 返回后端为 `RIFE` 时才接管调用方传入的 session，若返回 `QCOM_AHB` 或 `null`，调用方须自行负责关闭释放该 `AndroidMediaSession`。**

### 可选 QCOM 库构建与校验

`libmeguviz_qcom_motion.so` 独立于 RIFE 组合包。可通过 [QCOM 构建脚本](../src/android/qcom_motion/build_so.py) 使用 NDK `30.0.16248370` 复现已验证的 `arm64-v8a` 库（`--ndk-bin` 指向工具链 `bin` 目录，`--out` 指向不含目标文件的输出目录）。宿主按 ABI 将该 `.so` 放入 `app/src/main/jniLibs/arm64-v8a/`，不破坏 RIFE 组合布局。打包后应检查 ELF 16 KB 页对齐与 APK ZIP 对齐（参见 [Android 16 KB 页兼容说明](https://developer.android.com/guide/practices/page-sizes)）。

## 适用范围与验证说明

编译产物和离线包清单检验仅证明二进制格式与接口约束合规，不代表在真机设备上的 Android AHB 互操作、`SYNC_FD` 双向同步、16 KB 页运行、画质或音画同步已通过验证。正式上线前，须在目标真机和真实播放器窗口上完成连续播放、骤切、遮挡、Seek、切清晰度、热稳态及原帧对照实验。
