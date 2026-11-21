# 麒麟、Apple Metal 与 PowerVR 补帧适配

驱动报告用于明确探测条件，平台 API 用于明确能提供给后端的输入格式。是否启用补帧仍须以目标真机、实际缓冲格式、画质和帧预算通过实测为准。骁龙与天玑平台见 [Android GPU 补帧适配](ANDROID_GPU_COMPATIBILITY.md)。

## 麒麟 Mali 与 Maleoon

| 芯片 | 可核对的 GPU 与驱动证据 |
|---|---|
| 970 / 980 / 985 | 海思分别列 Mali-G72 12 核、G76 10 核、G77 8 核。 [970](https://www.hisilicon.com/en/products/kirin/kirin-flagship-chips/kirin-970) · [980](https://www.hisilicon.com/en/products/kirin/kirin-flagship-chips/kirin-980) · [985](https://www.hisilicon.com/en/products/kirin/mid-range-and-high-end/kirin-985) |
| 990 / 990 5G、990E | 990 与 990 5G 为 Mali-G76 16 核；990E 另列设备，Mate 40E 官方资料确认其 SoC。 [990](https://www.hisilicon.com/en/products/kirin/kirin-flagship-chips/kirin-990) · [990 5G](https://www.hisilicon.com/en/products/kirin/kirin-flagship-chips/kirin-990-5g) · [Mate 40E](https://consumer.huawei.com/cn/support/content/zh-cn15819087/) |
| 9000 / 9000E | 均为 Mali-G78，分别为 24 核与 22 核。 [9000](https://www.hisilicon.com/en/products/kirin/kirin-flagship-chips/kirin-9000) · [9000E](https://www.hisilicon.com/en/products/kirin/kirin-flagship-chips/kirin-9000e) |
| 9000S / 9020 / 9030 Pro | Khronos Vulkan 1.3 登记分别是 Maleoon 910 / HarmonyOS 4.0（#771）、Maleoon 920 / HarmonyOS 4.3（#872）、Maleoon 935 / EMUI 15.0.0（#973），均以所列合格配置为准。 [Khronos 登记](https://www.khronos.org/conformance/adopters/conformant-products/vulkan) |
| 9010 | Pura 70 Ultra 拆解确认 9010，公开测试机资料报告 Maleoon 910；适配时须再核验具体系统与驱动版本。 [TechInsights 拆解](https://www.techinsights.com/blog/hisilicon-kirin-9010-powers-huawei-pura-70-ultra) · [整机测试记录](https://www.notebookcheck.net/HiSilicon-Kirin-9010-Processor-Benchmarks-and-Specs.855471.0.html) |
| 9030 / 9030S / 9050 Pro | Mate 80 Pro 12 GB 版列 9030、16 GB 版列 9030 Pro；Pura 90 Pro 列 9030S，Mate XT 2 列 9050 Pro 与 HarmonyOS 7.0。公开规格尚未列出确切 GPU 型号。 [Mate 80 Pro](https://consumer.huawei.com/cn/phones/mate80-pro/specs/) · [Pura 90 Pro](https://consumer.huawei.com/cn/phones/pura90-pro/specs/) · [Mate XT 2](https://consumer.huawei.com/cn/phones/mate-xt-2-ultimate-design/specs/) |

P20 Pro 的 G72、Mate 30 的 G76 和 Mate 40 Pro 的 G78 公开驱动查询均显示支持 FP16 算术与 16 位存储，且 `shaderFloat64 = false`。Maleoon 920（PLR-AL00）公开报告默认 subgroup 为 64、可选 32–64；Maleoon 935（DEL-LX9）报告固定为 32。两者均声明支持 FP16 与 AHB r5，以及 `FOREIGN` 队列所有权和外部信号量 FD。现有 RIFE 可依据驱动能力门选择计算精度，但具体 AHB 用途与双向 `SYNC_FD` 仍须实机探测。自定义算子可从华为建议的 32 或 64 invocation 起步测试，并严格遵循 Vulkan 跨线程内存同步。 [G72](https://vulkan.gpuinfo.org/api/v3/getprofile.php?id=52871) · [G76](https://vulkan.gpuinfo.org/api/v3/getprofile.php?id=48043) · [G78](https://vulkan.gpuinfo.org/api/v3/getprofile.php?id=45080) · [Maleoon 920](https://vulkan.gpuinfo.org/api/v3/getprofile.php?id=36593) · [Maleoon 935](https://vulkan.gpuinfo.org/api/v3/getprofile.php?id=49321) · [华为优化指南](https://developer.huawei.com/consumer/en/doc/best-practices/bpta-maleoon-gpu-best-practices)

具备 Android 图形接口的整机优先验收 RIFE Vulkan + AHB，通过同配置资格选择 `HOST_WAIT` 或 `GPU_SYNC`。HarmonyOS 原生应用则应对接 `OH_NativeBuffer`、`OHNativeWindow`、`VK_OHOS_external_memory` 与对应围栏接口；通用 Vulkan 算子可复用，且运行时必须查询当前设备驱动而非仅看 SDK 头文件版本。 [HarmonyOS Vulkan](https://developer.huawei.com/consumer/en/doc/harmonyos-references/vulkan) · [OH_NativeBuffer](https://developer.huawei.com/consumer/en/doc/harmonyos-guides/native-buffer-guidelines) · [OHOS 外部内存](https://docs.vulkan.org/refpages/latest/refpages/source/VK_OHOS_external_memory.html)

Mate 20 X 的一份 Mali-G76 报告显示 GPU 时间戳能力为零。遇到无可用 GPU 时间戳的设备时，改用主机单调时钟测量端到端耗时，并将 GPU 阶段时间标记为不可用。 [Mate 20 X 驱动查询](https://vulkan.gpuinfo.org/displayreport.php?id=48372)

华为 Graphics Accelerate Kit 的 Vulkan 游戏超帧示例依赖真实帧颜色、深度与相机矩阵；仅握有两张解码视频帧的视频播放器仍应选用满足双帧输入模型的插帧算法。`VK_HUAWEI_subpass_shading` 可另行验证 render pass 内合成。 [华为 Vulkan 超帧](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/graphics-accelerate-fg-interpolation-vulkan) · [Khronos subpass shading](https://docs.vulkan.org/refpages/latest/refpages/source/VK_HUAWEI_subpass_shading.html)

## Apple Metal

Apple 官方特性表将 A8 归为 Apple2，A9/A10 为 Apple3，A11 为 Apple4，A12 为 Apple5，A13 为 Apple6，A14/M1 为 Apple7，A15/A16/M2 为 Apple8，A17 Pro/A18/M3/M4 为 Apple9，A19/M5 为 Apple10，M6 为 Apple11。运行时应检查系统 API 可用性、`MTLDevice.supportsFamily` 及各效果独立的设备查询接口。 [Metal 能力表](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf) · [Apple 设备探测](https://developer.apple.com/documentation/metal/detecting-gpu-features-and-metal-software-versions)

| 路径 | 适用条件与优化重点 |
| --- | --- |
| VideoToolbox 低延时插帧 | 适用于仅有前后解码帧的视频链路。自 iOS、iPadOS、macOS 26 起支持低延时配置，调用前检查 `isSupported`、尺寸和像素格式；后台建立会话并用缓冲池承载指定相位输出，保留媒体 PTS 并等待实际完成后归还。 [配置文档](https://developer.apple.com/documentation/videotoolbox/vtlowlatencyframeinterpolationconfiguration) · [实时视频示例](https://developer.apple.com/videos/play/wwdc2025/300/) |
| CVMetalTextureCache 与 Metal | 按像素平面映射可共享 `CVPixelBuffer`，保持 `CVMetalTexture` 与底层存储存活至 GPU 完成，并在 Metal 合成时校验格式、范围与同步。 [Core Video 与 Metal 指南](https://developer.apple.com/videos/play/wwdc2020/10090/) |
| MetalFX 插帧 | 面向能提供颜色、深度与运动矢量的渲染管线；分别用 `supportsDevice` 与 `supportsMetal4FX` 探测基础及 Metal 4 兼容性。 [MetalFX 描述符](https://developer.apple.com/documentation/metalfx/mtlfxframeinterpolatordescriptor) · [Apple 渲染用例](https://developer.apple.com/videos/play/wwdc2025/211/) |
| 自定义 Metal 算子与 MPSGraph | 依据管线 `threadExecutionWidth` 与 `maxTotalThreadsPerThreadgroup` 采用 SIMD-group 或线程组内归约以减少全局原子写；Metal 4 TensorOps 跨多代 Apple GPU 可用，A19/M5 每核神经硬件作为独立硬件层使用，模型算子融合与数值可在 MPSGraph 中验证。 [Metal 归约](https://developer.apple.com/videos/play/wwdc2022/10159/) · [A19/M5](https://developer.apple.com/videos/play/tech-talks/111432/) · [MPSGraph](https://developer.apple.com/videos/play/wwdc2020/10677/) |
| MoltenVK | 用作通用 Vulkan 算子的对比后端，须核对 portability enumeration/subset、实际启用特性和格式；与 `IOSurface`、`MTLTexture`、`MTLSharedEvent` 互操作遵循 `VK_EXT_metal_objects` 规范。 [MoltenVK 指南](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md) · [Metal 对象互操作](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_metal_objects.html) |

项目的独立 Apple 探针通过合成缓冲检验了 VideoToolbox 半时刻插帧的输出数量、像素、格式与 PTS，并用 Metal blit 验证了 Core Video 纹理映射及完成后释放；接入正式播放器前仍须在 Apple SDK 与真机环境下验证格式、画质、功耗与呈现表现。

## Imagination PowerVR

| 系列与设备 | 可核对的能力 |
|---|---|
| Rogue GE8320 / GE8322 | Helio P22/P35/G35 列 GE8320；LG LM-Q630 与 Gplus P10 原厂样本分别报告 GE8320、GE8322，均为 Vulkan 1.1.131、二维纹理上限 4096、subgroup 1，支持 FP16 算术及 16 位存储，不支持 FP64。 [P22](https://www.mediatek.com/products/smartphones/mediatek-helio-p22) · [P35](https://www.mediatek.com/products/smartphones/mediatek-helio-p35) · [G35](https://www.mediatek.com/products/smartphones/mediatek-helio-g35) · [GE8320 查询](https://vulkan.gpuinfo.org/displayreport.php?id=17489) · [GE8322 查询](https://vulkan.gpuinfo.org/displayreport.php?id=15921) |
| Rogue GM9446 | Helio P90 的 Ulefone 与 OPPO 样本分别报告 Vulkan 1.1.100 与 1.1.131，均具备 4096 纹理、subgroup 1、FP16 算术与存储，无 FP64。 [P90 规格](https://www.mediatek.com/products/smartphones/mediatek-helio-p90) · [Ulefone 查询](https://vulkan.gpuinfo.org/displayreport.php?id=15794) · [OPPO 查询](https://vulkan.gpuinfo.org/displayreport.php?id=12781) |
| A 系列与 BXM-8-256 | AXM/AXT 的 Vulkan 1.4 登记对应 x86_64 Ubuntu。天玑 930/7020/7025 部分机型搭载 BXM-8-256；Moto G54/7020 报告为 Vulkan 1.1.170，声明支持 AHB r3、`FOREIGN` 队列与 fence/semaphore FD。因 BXM-8-256 仍采用 RTZ 舍入，须单独验证 FP16 数值精度。 [A 系列](https://www.imaginationtech.com/products/gpu/img-a-series-gpu/) · [930 整机](https://en-us.support.motorola.com/app/answers/detail/a_id/174789/~/specifications--moto-g-power-5g-%282023%29) · [Moto G54 查询](https://vkdb.niconiconi.us/gpus/18) · [舍入说明](https://forums.imgtec.com/t/round-to-nearest-floating-point-rounding-mode/3206) · [Khronos 登记](https://www.khronos.org/conformance/adopters/conformant-products/vulkan) |
| CXT / Photon、DXT / DXTP、DXD / DXS | Photon 为 CXT 光追架构；DXT-8-256/DXTP 面向移动端，DXD 面向桌面与云端，DXS 面向车规安全。Android 16 与 Vulkan 1.4 已纳入 DDK 25.1 RTM2 作为后续测试起点。 [Photon](https://www.imaginationtech.com/products/gpu/graphics-architecture/powervr-photon/) · [DXT](https://www.imaginationtech.com/product/img-dxt-8-256/) · [D 系列定位](https://www.imaginationtech.com/news/taking-efficiency-up-a-level-with-latest-d-series-gpu-ip/) · [DDK 公告](https://blog.imaginationtech.com/imagination-gpus-now-support-vulkan-1.4-and-android-16) |

PowerVR 平台优先验证纹理/缓冲尺寸、RGBA8 AHB 与双向围栏，并准备较低处理尺寸和 `HOST_WAIT` 候选。Imagination 建议 Rogue 从 32 线程工作组、Volcanic 从 128 线程工作组起步实测（注意硬件任务粒度与 Vulkan subgroup 是两个独立概念），并独立校验 FP16 推理结果与坐标/阈值精度。 [Imagination 计算指南](https://docs.imgtec.com/performance-guides/compute-recommendations/html/topics/utilisation/maximise-utilisation.html) · [Android AHB](https://docs.vulkan.org/refpages/latest/refpages/source/VK_ANDROID_external_memory_android_hardware_buffer.html)

## 通用 Vulkan 试验

RIFE 支持通过 `-DMGR_ENABLE_GROUPED_STATISTICS=ON` 构建工作组局部归约的 Vulkan 统计变体。该开关默认关闭，仅作用于内置 shader；加载外置 shader pack 时继续使用原始统计 shader 与对应指纹契约，使用 `MeguvizShaderTools` 采集请求和制包时也应保持该开关为 `OFF`。

开启后，工作组（仍为 8×8）先在 1,564 字节共享内存中统计 391 个整数桶，再汇总非零桶写回全局缓冲，越界线程照常参与两次内存屏障，输入、采样点、桶布局与整数语义均保持不变。独立 shader 只需通用 32 位整数原子和工作组共享内存，最终应在同一设备、相同帧对下比对统计值、输出哈希、GPU 耗时与端到端延时，再决定构建配置。

Compute 的独立 RGB8 缩放算子另有 `binary64` 舍入契约，缺乏 FP64 的 GPU 继续沿用接口允许的回退实现；RIFE 的整数统计路径与该算子互相独立。
