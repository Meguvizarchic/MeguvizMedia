# Android 内容桥与官方组合候选

本次实现是功能候选。真实 Android GPU 的 AHB 互操作、SYNC_FD、颜色、图像质量、生命周期、显示与 A/V 资格均未由编译结果证明。

`MeguvizMedia::android` 保留宿主 OES producer，在同一个专用共享 GL context 中按调用方提供的 SurfaceTexture/crop/rotation/flip 矩阵绘制自有全分辨率 RGBA8 AHardwareBuffer。未知/HDR/保护内容不进入这个 SDR/full-range 接口。源 producer 的内容租期需覆盖 capture 调用；Media 的自有 history 则由 Compute 最终内容 lease 的 callback 固定池槽，AHB 对象引用不能授权覆盖。allocation ID/generation 在物理重建时改变，content ID 每次写入递增。池容量 3–12，满时返回 BACKPRESSURE。

HOST_WAIT 用 EGL fence 的显式 client wait，再消费 Compute 的真实 Vulkan acquire/release、GENERAL/FOREIGN ownership 桥。GPU_SYNC 消费 binary native SYNC_FD；GL draw 后把采样完成 fence 登记为下次 Compute acquire dependency。未支持时调用明确拒绝，宿主再选择已资格回退。默认依旧是未启用的新路径，不能依据函数存在自动宣称支持。

输入保护仍唯一归 Media：duplicate、静止、切镜分别按现有 native 采样位置与阈值抽取稀疏 RGBA 网格并读回。正常像素路径没有全帧 readPixels/CPU RGB/upload/download；稀疏保护读回的字节、同步和时间必须记成本。RIFE 模型相关 guard 由 RIFE 自己执行。输出 frame 中 diagnostics 绑定具体完成任务，不读取全局最近一次引擎数值。

AndroidMediaSession 在 GL owner 上捕获、呈现与收取结果。异步 submit 使用 RIFE 的已有 worker；native callback 只写最多三个结果的队列。准入 max-in-flight=1，使用最多 64 个成功的 admitted→sink-ready 联合服务时间样本的 p95 加安全余量。失败/取消不会作为快速成功样本。deadline admission 可独立关闭；过期输出拒绝和浅队列仍保留。旧 epoch 回调只物理回收，cancel/close 不提前释放输入或输出。C++ admission API 支持另外设置成本、百分位和队列上限。这里没有 actual-present 反馈，GLES draw/swap 只代表 sink submission。

`MediaNative` 是唯一 JNI class。frameCreate/configure/load/process/close 保留旧状态码、ByteBuffer 与保护工具语义，内部只调用 Compute/RIFE 公共 C API。LEGACY_CPU_BRIDGE 输入/输出拷贝为显式兼容成本，暂不据此宣称加速。新的 AHB 组合不得在同应用中再装独立 Compute/RIFE/ncnn DSO。

distribution/android 直接组合三个静态项目、固定 ncnn 与仍保留的 runtime glslang，只链接一次 static STL，导出来自公共 headers 的 C API 与固定 JNI class。`tools/build_android_composition.py` 为每 ABI 从链接输出创建 canonical payload，再执行一次 `--strip-unneeded`；AGP 的 media-rife-jni 消费同一份 bytes 并关闭再次 strip。普通 app 使用该 JNI AAR；宿主按需加载使用 native-payload + 无 native 的 media-api AAR，两种 native owner 互斥。manifest 的 finalized 仅表示二进制规范化完成，不表示资格通过。ARMv7 保留 CPU 像素往返 + Vulkan NN 的 legacy bridge（同样支持原 fp16/flow/guard 配置）；其 AHB/sync 与实际设备资格另列。

独立 sample-player 使用自身 MediaExtractor/MediaCodec → OES，已知 SDR 颜色时尝试相同 AHB identity 路径；不合格能力保留 OES 显示。该样例无音频，不是 A/V 验收。decoder AImageReader、SurfaceControl、HDR 都未混入默认路径。

必要编译命令：

```text
android/gradlew.bat :media-api:assembleRelease
python tools/build_android_composition.py --abi all --jvm-api android/media-api/build/outputs/aar/media-api-release.aar
android/gradlew.bat :media-rife-jni:assembleRelease :sample-player:assembleDebug
```

受控回退：应用停止新任务、取消/提升 epoch、继续 poll 直到 accepted 工作退役、释放 owned frames 与 GL 读取，close 成功后恢复宿主 OES/旧 ByteBuffer 路径。TIMEOUT/BUSY 时保留对象，不能强行释放 GPU 正在读取的资源。

## RIFE 与 QCOM 的统一 Android 入口

`FrameGenerationSession` 在同一个 EGL owner 上提供 OES 采集、异步中间帧提交、轮询、绘制和回收。RIFE 复用已准备好的 `AndroidMediaSession` 和原生 effect。QCOM 复用紧凑 AHB 亮度及现有 GLES warp，按当前输出尺寸映射源纹理坐标。QCOM JNI 仍是单独的可选库，宿主按 ABI 显式打包 `libmeguviz_qcom_motion.so`。缺少库、估计器或紧凑 AHB 预检不通过时，入口返回空会话。

宿主用本次播放的源尺寸、输出尺寸、颜色、帧率和中间帧预算构造 `FrameGenerationProfile`。完成真实设备的能力和画质验证后，再登记对应后端从帧对就绪到绘制提交的实测 p95，包含测量中使用的后端参数和余量。两路都满足预算时优先 RIFE。QCOM 还要求 8 位 BT.709 SDR。在尺寸、颜色、帧率或设备变化时重新建会话并验证资格。

宿主保有播放和音频时钟，先保障原帧。准入拒绝或结果过期时，直接使用宿主原帧路径。源 PTS 回跳或拖动时调用 `invalidate()`，释放旧引用并继续轮询已接受任务直到收到完成回执。不再使用该后端时，停止提交和释放引用，再在原 EGL owner 上关闭会话。关闭返回暂时繁忙或回收未证明时，保留 EGL owner 和上下文，继续完成回收。

绘制回执只证明 GLES 提交。宿主启用前仍需验证持续播放、骤切和遮挡画质、实际显示时序，以及拖动和切清晰度的音画同步。
