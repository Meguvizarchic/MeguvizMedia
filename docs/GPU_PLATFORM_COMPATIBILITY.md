# 麒麟、Apple Metal 与 PowerVR 补帧适配

驱动报告用于确定探测条件，平台 API 用于确定可以给后端的输入。补帧启用仍需要当前整机、实际缓冲、画质和帧预算通过验证。骁龙与天玑的其余机型见 [Android GPU 补帧适配](ANDROID_GPU_COMPATIBILITY.md)。

## 麒麟 Mali 与 Maleoon

| 芯片 | 可核对的 GPU 与驱动证据 |
|---|---|
| 970 / 980 / 985 | 海思分别列 Mali-G72 12 核、G76 10 核、G77 8 核。 [970](https://www.hisilicon.com/en/products/kirin/kirin-flagship-chips/kirin-970) · [980](https://www.hisilicon.com/en/products/kirin/kirin-flagship-chips/kirin-980) · [985](https://www.hisilicon.com/en/products/kirin/mid-range-and-high-end/kirin-985) |
| 990 / 990 5G、990E | 990 与 990 5G 为 Mali-G76 16 核。990E 另列设备，Mate 40E 官方资料确认其 SoC。 [990](https://www.hisilicon.com/en/products/kirin/kirin-flagship-chips/kirin-990) · [990 5G](https://www.hisilicon.com/en/products/kirin/kirin-flagship-chips/kirin-990-5g) · [Mate 40E](https://consumer.huawei.com/cn/support/content/zh-cn15819087/) |
| 9000 / 9000E | 均为 Mali-G78，分别为 24 核与 22 核。 [9000](https://www.hisilicon.com/en/products/kirin/kirin-flagship-chips/kirin-9000) · [9000E](https://www.hisilicon.com/en/products/kirin/kirin-flagship-chips/kirin-9000e) |
| 9000S / 9020 / 9030 Pro | Khronos Vulkan 1.3 登记分别是 Maleoon 910 / HarmonyOS 4.0（#771）、Maleoon 920 / HarmonyOS 4.3（#872）、Maleoon 935 / EMUI 15.0.0（#973），均以所列合格配置为范围。 [Khronos 登记](https://www.khronos.org/conformance/adopters/conformant-products/vulkan) |
| 9010 | Pura 70 Ultra 拆解确认 9010，公开测试机资料报告 Maleoon 910。适配时再核验具体系统与驱动。 [TechInsights 拆解](https://www.techinsights.com/blog/hisilicon-kirin-9010-powers-huawei-pura-70-ultra) · [整机测试记录](https://www.notebookcheck.net/HiSilicon-Kirin-9010-Processor-Benchmarks-and-Specs.855471.0.html) |
| 9030 / 9030S / 9050 Pro | Mate 80 Pro 的 12 GB 版列 9030，16 GB 版列 9030 Pro。Pura 90 Pro 列 9030S，Mate XT 2 列 9050 Pro 与 HarmonyOS 7.0。这几款的公开整机规格未列确切 GPU。 [Mate 80 Pro](https://consumer.huawei.com/cn/phones/mate80-pro/specs/) · [Pura 90 Pro](https://consumer.huawei.com/cn/phones/pura90-pro/specs/) · [Mate XT 2](https://consumer.huawei.com/cn/phones/mate-xt-2-ultimate-design/specs/) |

P20 Pro 的 G72、Mate 30 的 G76 和 Mate 40 Pro 的 G78 公开查询均报告 FP16 算术与 16 位存储可用，且 `shaderFloat64=false`。Maleoon 920 PLR-AL00 公开驱动报告默认 subgroup 为 64、可选范围 32–64，Maleoon 935 DEL-LX9 报告固定 32。两者均报告 FP16 与 AHB r5，并广告 FOREIGN 队列所有权及外部 semaphore FD。现有 RIFE 可用驱动能力门选择精度，具体 AHB 用途和双向 SYNC_FD 仍需验证。自定义算子可从华为建议的 32 或 64 invocation 开始测，跨线程写读仍遵循实际 Vulkan 内存同步。 [G72](https://vulkan.gpuinfo.org/api/v3/getprofile.php?id=52871) · [G76](https://vulkan.gpuinfo.org/api/v3/getprofile.php?id=48043) · [G78](https://vulkan.gpuinfo.org/api/v3/getprofile.php?id=45080) · [Maleoon 920](https://vulkan.gpuinfo.org/api/v3/getprofile.php?id=36593) · [Maleoon 935](https://vulkan.gpuinfo.org/api/v3/getprofile.php?id=49321) · [华为优化指南](https://developer.huawei.com/consumer/en/doc/best-practices/bpta-maleoon-gpu-best-practices)

能提供 Android 图形接口的整机先验收现有 RIFE Vulkan + AHB，SDK 通过同配置资格选择 HOST_WAIT 或 GPU_SYNC。HarmonyOS 原生应用的资源桥应分别对接 `OH_NativeBuffer`、`OHNativeWindow`、`VK_OHOS_external_memory` 和当前系统的围栏契约。通用 Vulkan 算子可作为复用候选。SDK 头文件版本只说明编译接口，应用运行时仍查询当前驱动。 [HarmonyOS Vulkan](https://developer.huawei.com/consumer/en/doc/harmonyos-references/vulkan) · [OH_NativeBuffer](https://developer.huawei.com/consumer/en/doc/harmonyos-guides/native-buffer-guidelines) · [OHOS 外部内存](https://docs.vulkan.org/refpages/latest/refpages/source/VK_OHOS_external_memory.html)

Mate 20 X 的一份 Mali-G76 报告中，GPU 时间戳相关能力为零。无可用 GPU 时间戳时，按主机单调时钟验证端到端开销，GPU 阶段时间标为不可用。 [Mate 20 X 驱动查询](https://vulkan.gpuinfo.org/displayreport.php?id=48372)

华为 Graphics Accelerate Kit 的 Vulkan 游戏超帧示例需要真实帧颜色、深度和相机矩阵。仅有两张解码视频帧的应用继续使用满足本项目输入契约的算法。`VK_HUAWEI_subpass_shading` 可另做 render pass 内合成验证。 [华为 Vulkan 超帧](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/graphics-accelerate-fg-interpolation-vulkan) · [Khronos subpass shading](https://docs.vulkan.org/refpages/latest/refpages/source/VK_HUAWEI_subpass_shading.html)

## Apple Metal

Apple 官方表列出 A8 为 Apple2，A9/A10 为 Apple3，A11 为 Apple4，A12 为 Apple5，A13 为 Apple6。A14/M1 为 Apple7，A15/A16/M2 为 Apple8，A17 Pro/A18/M3/M4 为 Apple9，A19/M5 为 Apple10，M6 为 Apple11。运行时检查系统 API 可用性、`MTLDevice.supportsFamily` 和效果独立的设备查询。 [Metal 能力表](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf) · [Apple 设备探测](https://developer.apple.com/documentation/metal/detecting-gpu-features-and-metal-software-versions)

| 路径 | 使用条件和优化点 |
|---|---|
| VideoToolbox 低延时插帧 | 用于已有前后解码帧的链路。具体低延时配置和参数从 iOS、iPadOS、macOS 26 起，调用前检查 `isSupported`、尺寸与像素格式。后台建立会话，前一帧与当前帧产生指定相位的输出，目标缓冲可使用池。保存媒体 PTS，并在实际完成后再归还输出。 [配置](https://developer.apple.com/documentation/videotoolbox/vtlowlatencyframeinterpolationconfiguration) · [实时视频示例](https://developer.apple.com/videos/play/wwdc2025/300/) |
| CVMetalTextureCache 与 Metal | 按实际像素面映射可共享的 CVPixelBuffer，保持 CVMetalTexture 和底层存储至 GPU 完成，参与 Metal 合成时验证像素格式、范围和同步。 [Apple Core Video 与 Metal 指南](https://developer.apple.com/videos/play/wwdc2020/10090/) |
| MetalFX 插帧 | 面向能提供颜色、深度和运动矢量的渲染器。`supportsDevice` 和 `supportsMetal4FX` 分别验证效果及使用 Metal 4 的兼容性。UI 可独立合成。 [MetalFX 描述符](https://developer.apple.com/documentation/metalfx/mtlfxframeinterpolatordescriptor) · [Apple 渲染用例](https://developer.apple.com/videos/play/wwdc2025/211/) |
| 自定义 Metal 算子与 MPSGraph | 依据编译后管线的 `threadExecutionWidth`、`maxTotalThreadsPerThreadgroup` 使用 SIMD-group 或线程组内归约，再减少全局原子写。Metal 4 TensorOps 可跨多代 Apple GPU 使用，A19/M5 的每核神经硬件是单独的硬件层。模型图可在 MPSGraph 中做具体算子支持、融合和数值对照。 [Metal 归约](https://developer.apple.com/videos/play/wwdc2022/10159/) · [A19/M5](https://developer.apple.com/videos/play/tech-talks/111432/) · [MPSGraph](https://developer.apple.com/videos/play/wwdc2020/10677/) |
| MoltenVK | 作为通用 Vulkan 算子的独立比较后端，检查 portability enumeration/subset、实际 enabled 功能和所需格式。IOSurface、MTLTexture、MTLSharedEvent 互操作按具体 `VK_EXT_metal_objects` 契约验证。 [MoltenVK 指南](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md) · [Metal 对象互操作](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_metal_objects.html) |

项目的独立 Apple 探针生成两张合成缓冲并尝试 VideoToolbox 半时刻插帧，检查输出数量、像素、格式和 PTS，并用 Metal blit 验证 Core Video 纹理映射及完成后释放。接入播放器前还需在 Apple SDK 上构建和在实际设备上验证格式、画质、能耗与呈现。

## Imagination PowerVR

| 系列与设备 | 可核对的能力 |
|---|---|
| Rogue GE8320 / GE8322 | Helio P22/P35/G35 的规格列 GE8320。LG LM-Q630 和 Gplus P10 原厂样本分别报告 GE8320、GE8322，Vulkan 1.1.131、二维纹理上限 4096、subgroup 1，FP16 算术及 16 位存储可用，FP64 缺失。 [P22](https://www.mediatek.com/products/smartphones/mediatek-helio-p22) · [P35](https://www.mediatek.com/products/smartphones/mediatek-helio-p35) · [G35](https://www.mediatek.com/products/smartphones/mediatek-helio-g35) · [GE8320 查询](https://vulkan.gpuinfo.org/displayreport.php?id=17489) · [GE8322 查询](https://vulkan.gpuinfo.org/displayreport.php?id=15921) |
| Rogue GM9446 | Helio P90 的 Ulefone 与 OPPO 样本报告 Vulkan 1.1.100 和 1.1.131，均为 4096 纹理、subgroup 1、有 FP16 算术和存储、无 FP64。 [P90 规格](https://www.mediatek.com/products/smartphones/mediatek-helio-p90) · [Ulefone 查询](https://vulkan.gpuinfo.org/displayreport.php?id=15794) · [OPPO 查询](https://vulkan.gpuinfo.org/displayreport.php?id=12781) |
| A 系列与 BXM-8-256 | AXM/AXT 的 Vulkan 1.4 登记是 x86_64 Ubuntu。天玑 930/7020/7025 的部分整机使用 BXM-8-256，Moto G54/7020 原厂报告 Vulkan 1.1.170，广告 AHB r3、FOREIGN 和 fence/semaphore FD。Imagination 工程师记录 BXM-8-256 仍采用 RTZ，应独立验证 FP16 输出与舍入。 [A 系列](https://www.imaginationtech.com/products/gpu/img-a-series-gpu/) · [930 整机](https://en-us.support.motorola.com/app/answers/detail/a_id/174789/~/specifications--moto-g-power-5g-%282023%29) · [Moto G54 查询](https://vkdb.niconiconi.us/gpus/18) · [舍入说明](https://forums.imgtec.com/t/round-to-nearest-floating-point-rounding-mode/3206) · [Khronos 登记](https://www.khronos.org/conformance/adopters/conformant-products/vulkan) |
| CXT / Photon、DXT / DXTP、DXD / DXS | Photon 是 CXT 的射线追踪架构。DXT-8-256 与 DXTP 有移动 IP 资料，DXD 面向桌面和云，DXS 面向汽车安全。Android 16 与 Vulkan 1.4 进入 DDK 25.1 RTM2，作为驱动发布起点列入测试。 [Photon](https://www.imaginationtech.com/products/gpu/graphics-architecture/powervr-photon/) · [DXT](https://www.imaginationtech.com/product/img-dxt-8-256/) · [D 系列定位](https://www.imaginationtech.com/news/taking-efficiency-up-a-level-with-latest-d-series-gpu-ip/) · [DDK 公告](https://blog.imaginationtech.com/imagination-gpus-now-support-vulkan-1.4-and-android-16) |

PowerVR 优先验证实际纹理/缓冲尺寸、RGBA8 AHB 和双向围栏，准备较低处理尺寸与 HOST_WAIT 候选。Imagination 建议 Rogue 从 32 线程工作组起测，Volcanic 从 128 起测，两者应实际测量，且硬件任务大小和 Vulkan subgroup 是两个独立的量。FP16 推理及数据搬运独立对照，保留坐标和阈值的精度。 [Imagination 计算指南](https://docs.imgtec.com/performance-guides/compute-recommendations/html/topics/utilisation/maximise-utilisation.html) · [Android AHB](https://docs.vulkan.org/refpages/latest/refpages/source/VK_ANDROID_external_memory_android_hardware_buffer.html)

## 通用 Vulkan 试验

RIFE 可用 `-DMGR_ENABLE_GROUPED_STATISTICS=ON` 构建使用工作组归约的 Vulkan 变体。默认关闭，开启时只用于内置 shader，加载外置 shader pack 的计划继续使用原始统计 shader 和现有指纹契约。用 ShaderTools 采集和制包时使用 OFF，使统计 shader 的采集源与外置包启用时相同。

这个变体把 391 个整数桶分组归约。工作组先在 1,564 字节共享内存统计，汇总非零桶后再写全局。保持输入、采样点、桶布局和整数语义，越界线程仍参加两次屏障。工作组仍为 8×8，独立 shader 只需要通用 32 位整数原子和工作组共享内存。验收比较相同帧对、保护统计、采样输出哈希、GPU 和端到端延时，再决定当前设备的构建策略。

Compute 的独立 RGB8 resize 另有 binary64 舍入契约。缺少 FP64 的 GPU 继续采用接口允许的回退。替换该算子需要先解决半整数边界上的输出等价，RIFE 的整数统计属于独立路径。
