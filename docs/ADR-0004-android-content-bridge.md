# Android 内容桥与官方组合候选

本次实现是功能候选。真实 Android GPU 的 AHB 互操作、SYNC_FD、颜色、图像质量、生命周期、显示与 A/V 资格均未由编译结果证明。

`MeguvizMedia::android` 保留宿主 OES producer，在同一个专用共享 GL context 中按调用方提供的 SurfaceTexture/crop/rotation/flip 矩阵绘制自有全分辨率 RGBA8 AHardwareBuffer。未知/HDR/保护内容不进入这个 SDR/full-range 接口。源 producer 的内容租期需覆盖 capture 调用；Media 的自有 history 则由 Compute 最终内容 lease 的 callback 固定池槽，AHB 对象引用不能授权覆盖。allocation ID/generation 在物理重建时改变，content ID 每次写入递增。池容量 3–12，满时返回 BACKPRESSURE。

HOST_WAIT 用 EGL fence 的显式 client wait，再消费 Compute 的真实 Vulkan acquire/release、GENERAL/FOREIGN ownership 桥。GPU_SYNC 消费 binary native SYNC_FD；GL draw 后把采样完成 fence 登记为下次 Compute acquire dependency。未支持时调用明确拒绝，宿主再选择已资格回退。默认依旧是未启用的新路径，不能依据函数存在自动宣称支持。

RIFE 的 `AndroidMediaSession.compare()` 提供重复帧、静止和切镜判定，使用稀疏 RGBA 网格读回。同步插值路径也会执行这项采样。测量时计入读回字节、同步与耗时。RIFE 模型相关保护由 RIFE 执行，输出帧的诊断数据绑定到对应任务。

AndroidMediaSession 在 GL owner 上捕获、呈现与收取结果。异步 submit 使用 RIFE 的已有 worker；native callback 只写最多三个结果的队列。准入 max-in-flight=1，使用最多 64 个成功的 admitted→sink-ready 联合服务时间样本的 p95 加安全余量。失败/取消不会作为快速成功样本。deadline admission 可独立关闭；过期输出拒绝和浅队列仍保留。旧 epoch 回调只物理回收，cancel/close 不提前释放输入或输出。C++ admission API 支持另外设置成本、百分位和队列上限。这里没有 actual-present 反馈，GLES draw/swap 只代表 sink submission。

Compute、Media 和 RIFE 的 Android 组合通过 `MediaNative` 暴露 JNI。frameCreate/configure/load/process/close 保留旧状态码、ByteBuffer 和保护工具语义，内部调用 Compute/RIFE 公共 C API。测量 LEGACY_CPU_BRIDGE 时计入输入与输出拷贝。同一个宿主只使用一份 Compute/RIFE/ncnn 原生库组合。

distribution/android 直接组合三个静态项目、固定 ncnn 与仍保留的 runtime glslang，只链接一次 static STL，导出来自公共 headers 的 C API 与固定 JNI class。`tools/build_android_composition.py` 为每 ABI 从链接输出创建 canonical payload，再执行一次 `--strip-unneeded`；AGP 的 media-rife-jni 消费同一份 bytes 并关闭再次 strip。普通 app 使用该 JNI AAR；宿主按需加载使用 native-payload + 无 native 的 media-api AAR，两种 native owner 互斥。manifest 的 finalized 仅表示二进制规范化完成，不表示资格通过。ARMv7 保留 CPU 像素往返 + Vulkan NN 的 legacy bridge（同样支持原 fp16/flow/guard 配置）；其 AHB/sync 与实际设备资格另列。

独立 sample-player 使用自身 MediaExtractor/MediaCodec → OES，已知 SDR 颜色时尝试相同 AHB identity 路径；不合格能力保留 OES 显示。该样例无音频，不是 A/V 验收。decoder AImageReader、SurfaceControl、HDR 都未混入默认路径。

在 MeguvizMedia 仓库根目录运行以下命令。Android 模块使用 JDK 17 和 Android SDK 37，`minSdk` 为 27。组合构建需要同级的 MeguvizCompute、MeguvizRIFE 和 MeguvizShaderTools 源码目录。工具链安装位置不同的环境可给 Python 命令传入 `--ndk`、`--cmake` 和 `--ninja`。`meguviz.compositionDir` 相对于 `android` 项目根目录解析。

```text
android/gradlew.bat -p android :media-api:assembleRelease :media-api:testDebugUnitTest
python tools/build_android_composition.py --abi all --jvm-api android/media-api/build/outputs/aar/media-api-release.aar
android/gradlew.bat -p android -Pmeguviz.compositionDir=../artifacts/M6/android-composition :media-rife-jni:assembleRelease :sample-player:assembleDebug
```

受控回退：应用停止新任务、取消/提升 epoch、继续 poll 直到 accepted 工作退役、释放 owned frames 与 GL 读取，close 成功后恢复宿主 OES/旧 ByteBuffer 路径。TIMEOUT/BUSY 时保留对象，不能强行释放 GPU 正在读取的资源。

## RIFE 与 QCOM 的统一 Android 入口

设备系列和公开驱动证据见 [Android GPU 补帧适配](ANDROID_GPU_COMPATIBILITY.md)。

`FrameGenerationSession` 在持有当前 EGL 上下文的线程上完成 OES 采集、双帧中点提交、轮询、绘制和回收。RIFE 复用已准备好的 `AndroidMediaSession`。QCOM 复用紧凑 AHB 亮度及 GLES warp，并按输出尺寸映射源纹理坐标。QCOM 绘制到当前 EGL 绘制面对应的默认帧缓冲。后续宿主绘制按自己的要求重新设置 GLES 状态，窗口交换由宿主负责。

### 接入顺序

1. **准备。** 根据源与输出尺寸、颜色、源帧和显示周期及中间帧预算创建 `FrameGenerationProfile`。在当前设备上完成能力和画质验证，再用 `profile.qualify()` 登记对应后端从帧对就绪到绘制提交的实测 p95。资格与创建它的 profile 实例绑定，也应随测量使用的后端配置一起管理。显示周期须短于源帧周期，两路均要求 8 位 SDR，QCOM 还要求 BT.709。尺寸、颜色、帧率或设备变化时重新建立会话并验证资格。
2. **开启。** 在 EGL owner 上、逐帧循环之外调用 `open(profile, rife, preparedRife, flowScale, qcom)`。两路都合格时优先 RIFE。`preparedRife` 仅在选中 RIFE 时调用，返回的独占会话由统一会话接管，返回 `null` 则尝试已合格的 QCOM。缺少 QCOM 库、估计器或 AHB 预检失败会使 QCOM 路径不可用，`open()` 返回 `null` 时使用宿主原帧路径。
3. **捕获和调度。** 在 EGL owner 上更新 `SurfaceTexture`，并为这次捕获取新的变换矩阵。`capture()` 的 `ptsUs` 是当前解码帧的媒体 PTS，单位为微秒。`trySubmit()` 的 `deadlineNs` 则是与 `System.nanoTime()` 同一时间基准的绝对截止时间。给源帧分配递增 ID，提交来自同一代且 PTS 递增的两个原帧。`capture()` 返回 `null` 时跳过补帧并继续原帧呈现。 [Android SurfaceTexture 参考](https://developer.android.com/reference/android/graphics/SurfaceTexture)
4. **取帧和释放。** 在宿主已有的调度节拍上调用 `poll()`，同时推进底层资源回收。返回 `null` 表示当前没有回执。已接受任务到达截止时间且尚无回执时，`poll()` 会请求取消一次。任务仍占用槽位直至退役，过期输出会被丢弃。有输出帧时按宿主音视频时钟调度 `draw()`，并以 profile 的输出尺寸绘制。没有可用输出或绘制遇到 `WindowBackend.Backpressure` 时，由宿主兜底呈现原帧。`capture()` 和 `poll()` 交付的帧都需要在 owner 上 `release()` 一次，包括被跳过的帧。

`trySubmit()` 提供逐帧准入原因。`ACCEPTED` 带正数 `jobId`，`IN_FLIGHT` 表示前一任务占用槽位，`DEADLINE` 表示剩余预算不足，`INPUT_NOT_READY` 表示输入尚未就绪，`BACKEND_REJECTED` 表示后端本次未接受。跳过后继续原帧，按宿主节拍再判断下一次机会。旧的 `submit()` 仍返回正数任务 ID 或 `0`。

```java
FrameGenerationSession.Submission submission = session.trySubmit(left, right, deadlineNs);
if (submission.jobId != 0) {
    pendingJobId = submission.jobId;
}
```

拖动或 PTS 回跳时调用 `invalidate()`，释放旧帧引用，并继续轮询已接受的任务直至终态回执。切清晰度或离开播放时停止提交，在同一 EGL owner 上释放帧引用后调用 `close()`。关闭可能等待后端回收，安排在逐帧循环之外。开始关闭后由 `close()` 推进回收。若暂时繁忙或回收未证明，保持会话、owner 与 EGL 上下文，待下一次 owner 调度再试。新会话就绪前恢复宿主原帧路径。

### 选择已验证的 RIFE 配置

对每个已完成画质和负载验证的候选，用 `qualifyRife()` 登记处理宽高、AHB 同步路径、固定 flow 倍率和实测 p95。省略处理宽高时使用本 profile 的源尺寸。用 `selectRife()` 过滤过期和超预算资格，再按处理像素数、倍率和延时选择，完全同分优先 HOST_WAIT。

用该资格的 `rifeWidth`、`rifeHeight` 和 `rifePath` 异步准备会话，另外传入原始源尺寸。提交后的会话先在 EGL owner 上调用一次 `probe()`，再将会话和资格交给 `openQualified()`。精确入口会核对已成功的 AHB 探测、源与处理尺寸、同步路径及是否为模型会话，并使用资格中的 `flowScale`。GPU_SYNC 资格需要双向围栏。只有返回 RIFE 时才接管已准备会话。调用方持有模型、精度、优化参数及设备代际的同配置实测记录，GL owner 上的打开操作不重新探测或等待后台工作。

原有 `qualify()` 和 `open()` 供既有调用者使用。精确资格走 `openQualified()`。在模型、处理或输出尺寸、驱动或负载条件变化后重新验证。

### 可选 QCOM 打包与验收

`libmeguviz_qcom_motion.so` 独立于 RIFE 组合。可通过 [QCOM 构建脚本](../src/android/qcom_motion/build_so.py) 用固定的 NDK `30.0.16248370` 复现已验证的 `arm64-v8a` 文件。`--ndk-bin` 指向 NDK 的 `toolchains/llvm/prebuilt/<host-tag>/bin`，`--out` 指向不含同名库的构建目录。宿主按 ABI 把库放到 `app/src/main/jniLibs/arm64-v8a/` 或其对应源集，保持 RIFE 组合的 manifest 和原生文件布局。最终发布包需检查原生库的 ELF 对齐，以及未压缩原生库在 APK 内的 ZIP 对齐。 [Android 16 KB 页兼容说明](https://developer.android.com/guide/practices/page-sizes)

绘制回执只证明 GLES 提交。离屏测试可验证出帧、缩放、取消和回收。宿主启用前还需用真实窗口与音频测持续播放、骤切和遮挡画质、原帧连续性、功耗、拖动及切清晰度时的显示时序与音画同步。比较后端时记录投入帧对、准入跳过、过期结果和实际呈现的补帧数，与同素材的原帧播放对照。
