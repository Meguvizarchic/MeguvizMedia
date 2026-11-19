# Android GPU 补帧适配

这份索引用于挑选测试设备和后端候选，覆盖近几代骁龙旗舰、常用 8s 和 7 系列，以及天玑旗舰和主流 8、7、6 系列。芯片规格确认硬件系列，Khronos 登记确认特定系统的合格配置，公开驱动报告说明某台整机报告过哪些能力。实际启用以当前设备的资源探测、画质和工作负载为准。

## 骁龙

| 芯片与 GPU | 公开证据与适配起点 |
|---|---|
| 8 Elite Gen 6 / Extreme Gen 6（SM8950 / SM8975，Adreno 845 / 850） | Khronos #1005 / #1006 均为 Android 17 Vulkan 1.4。按实际设备探测 RIFE 和 QCOM 块匹配，矩阵核心与 Tile Memory 做单独验证。 [Qualcomm 图形资料](https://www.qualcomm.com/developer/blog/2026/10/same-day-snapdragon-8-elite-gen-6-upstream-linux-support) · [Khronos 登记](https://www.khronos.org/conformance/adopters/conformant-products/vulkan) |
| 8 Elite Gen 5（SM8850，Adreno 840） | 项目已验证一支原厂驱动上的 RIFE AHB 和 QCOM。协作矩阵卷积编译回退绑定 Vulkan 厂商、设备及驱动版本。 [Qualcomm 规格](https://www.qualcomm.com/content/dam/qcomm-martech/dm-assets/documents/Snapdragon-8-Elite-Gen-5-product-brief.pdf) · [图像处理用例](https://github.com/KhronosGroup/Vulkan-ValidationLayers/issues/11339) |
| 8 Gen 5（SM8845，Adreno 829） | OnePlus 15R 的报告含与项目 840 样本相同的 Qualcomm driver build。适配需保留设备 ID，工作负载资格独立验证。 [产品规格](https://www.qualcomm.com/content/dam/qcomm-martech/dm-assets/documents/Product-Brief-Snapdragon-8-Gen-5.pdf) · [整机报告](https://fauxvkdb.canonspike.com/device/oneplus-cph2767) |
| 8 Elite（SM8750，Adreno 830） | Khronos #844 为 Android 15 Vulkan 1.4。两份三星与 OnePlus 原厂报告为 Vulkan 1.3.284，均广告 QCOM 图像处理 v1/v2 和 `VK_NV_optical_flow`。Qualcomm 实机用例记录了 v2 的格式和边界约束。 [Khronos 登记](https://www.khronos.org/conformance/adopters/conformant-products/vulkan) · [三星报告](https://fauxvkdb.canonspike.com/gpu/5143-44050001/d2a62324160c6858) · [OnePlus 报告](https://fauxvkdb.canonspike.com/gpu/5143-44050001/e6d5d8464dccd90e) · [实机用例](https://github.com/KhronosGroup/Vulkan-ValidationLayers/issues/11339) |
| 8 Gen 3（SM8650，Adreno 750） | Galaxy S24 Ultra SM-S928U1 的 Vulkan 1.3.128 样本报告 QCOM v1 块匹配和 64×64 最大块。光流功能位与队列可见，但该报告的光流网格和尺寸字段为零，因此光流仍列待测。 [Qualcomm 规格](https://docs.qualcomm.com/doc/87-71408-1/87-71408-1_REV_G_Snapdragon_8_gen_3_Mobile_Platform_Product_Brief.pdf) · [整机查询](https://vulkan.gpuinfo.org/displayreport.php?id=44583) |
| 8 Gen 2（SM8550，Adreno 740） | 本项目已验证 QCOM v1 和 RIFE AHB，Khronos #727 为 Android 13 Vulkan 1.3。 [Qualcomm 规格](https://www.qualcomm.com/content/dam/qcomm-martech/dm-assets/documents/Snapdragon-8-Gen-2-Product-Brief.pdf) · [Khronos 登记](https://www.khronos.org/conformance/adopters/conformant-products/vulkan) |
| 8 Gen 1 / 8+ Gen 1（SM8450 / SM8475，Adreno 730 系列） | 产品规格列 Vulkan 1.1。保留 Vulkan 1.1 加 SPIR-V 1.4 与浮点控制扩展的兼容门，QCOM 块匹配与 AHB 同步另探测。 [8 Gen 1](https://www.qualcomm.com/content/dam/qcomm-martech/dm-assets/documents/snapdragon-8-gen-1-mobile-platform-product-brief.pdf) · [8+ Gen 1](https://www.qualcomm.com/content/dam/qcomm-martech/dm-assets/documents/Snapdragon-8-plus-Gen-1-Product-Brief.pdf) |
| 888 / 888+（Adreno 660） | 两份官方规格列 Vulkan 1.1。先测小处理尺寸和 HOST_WAIT，保持原帧档。 [888](https://www.qualcomm.com/products/snapdragon-888-5g-mobile-platform) · [888+](https://www.qualcomm.com/smartphones/products/8-series/snapdragon-888-plus-5g-mobile-platform) |
| 8s Gen 4 / 8s Gen 3（SM8735 / SM8635） | POCO F7 原厂报告广告 QCOM v1/v2。POCO F6 的关联原厂报告广告 v1，设备页与查询页的 GPU 显示名有差异。 [F7 报告](https://fauxvkdb.canonspike.com/gpu/5143-44030000/3cc5adba679c69ac) · [F6 设备](https://fauxvkdb.canonspike.com/device/xiaomi-24069pc21g) · [F6 关联报告](https://fauxvkdb.canonspike.com/gpu/5143-43030b00/3753930a09bdd192) |
| 7+ Gen 3 / 7 Gen 4（SM7675 / SM7750，Adreno 732 / 722） | Android Vulkan 1.3 登记 #774 / #903。优先验证通用 RIFE AHB 与较小处理尺寸，再实测驱动允许的专有路径。 [Khronos 登记](https://www.khronos.org/conformance/adopters/conformant-products/vulkan) · [Ace 3V 查询](https://www.deviceinfohw.ru/devices/item.php?item=109713) · [realme 查询](https://www.deviceinfohw.ru/devices/item.php?item=118574) |
| 7+ Gen 2 / 7 Gen 3 / 7s Gen 4 | 厂商分别列 Vulkan 1.1、1.3、1.3。通用 RIFE 与原帧回退按 OEM 驱动和帧预算验证。 [7+ Gen 2](https://docs.qualcomm.com/doc/87-43682-1/87-43682-1_REV_A_Snapdragon_7__Gen_2_Mobile_Platform_Product_Brief.pdf) · [7 Gen 3](https://docs.qualcomm.com/doc/87-64372-1/87-64372-1_REV_A_Snapdragon_7_gen_3_Mobile_Platform_Product_Brief.pdf) · [7s Gen 4](https://www.qualcomm.com/content/dam/qcomm-martech/dm-assets/documents/Snapdragon-7s-Gen-4-product-brief.pdf) |
| 6 Gen 1 / 3 / 4 与 6s Gen 3 / 4 | 保持通用探测入口，准备低处理尺寸和原帧作为候选。逐整机确认 Vulkan、围栏与预算，目录仅作测试设备索引。 [Qualcomm 6 系列目录](https://www.qualcomm.com/smartphones/products/6-series.md) |

QCOM v1 按 `textureBlockMatch`、块尺寸、实际格式用法、计算队列和 shader 版本创建。v2 Gather/Window 还要验证各自的格式及边界语义，v3 提供了双输入扩展 `CLAMP_TO_EDGE` 及用于普通 SAD/SSD 的补充格式位。840 的 Gather 实验通过输出对照，未测到提交等待收益，当前配置继续使用 v1。 [v1](https://docs.vulkan.org/refpages/latest/refpages/source/VK_QCOM_image_processing.html) · [v2](https://docs.vulkan.org/refpages/latest/refpages/source/VK_QCOM_image_processing2.html) · [v3](https://docs.vulkan.org/features/latest/features/proposals/VK_QCOM_image_processing3.html)

Qualcomm QHWI 使用 `VK_NV_optical_flow`。光流的实际准入要验证队列、输出网格、支持尺寸、精确输入输出格式及 session，再测运动边界与遮挡画质。Tile Memory 可试验同一提交内使用的临时缓冲。持久 AHB 与跨提交张量使用常规内存。 [Qualcomm QHWI](https://github.com/qualcomm/qhwi) · [光流属性](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceOpticalFlowPropertiesNV.html) · [Tile Memory](https://docs.vulkan.org/refpages/latest/refpages/source/VK_QCOM_tile_memory_heap.html)

## 天玑

| 芯片 | 公开 GPU 规格 |
|---|---|
| 9000 / 9000+ | Mali-G710 MC10。 [9000](https://www.mediatek.com/products/smartphones/mediatek-dimensity-9000) · [9000+](https://www.mediatek.com/products/smartphones/mediatek-dimensity-9000plus) |
| 9200 / 9200+ | Immortalis-G715。 [9200](https://www.mediatek.com/dimensity-9200) · [9200+](https://www.mediatek.com/dimensity-9200-plus) |
| 9300 / 9300+ / 9400e | Immortalis-G720。 [9300](https://www.mediatek.com/press-room/mediateks-new-all-big-core-design-for-flagship-dimensity-9300-chipset-maximizes-smartphone-performance-and-efficiency) · [9300+](https://www.mediatek.com/products/smartphones/mediatek-dimensity-9300-plus) · [9400e](https://www.mediatek.com/products/smartphones/mediatek-dimensity-9400e) |
| 9400 / 9400+ / 9500s | Immortalis-G925，9500s 有 11 或 12 核配置。 [9400](https://www.mediatek.com/press-room/mediateks-dimensity-9400-flagship-soc-offers-extreme-performance-and-efficiency-for-the-latest-ai-experiences) · [9400+](https://www.mediatek.com/zh-cn/products/smartphones/mediatek-dimensity-9400-plus) · [9500s](https://www.mediatek.com/products/smartphones/mediatek-dimensity-9500s) |
| 9500 / 9600M | Mali-G1 Ultra MC12。 [9500](https://www.mediatek.com/products/smartphones/mediatek-dimensity-9500) · [9600M](https://www.mediatek.com/products/smartphones/dimensity-9600m) |
| 9600 Pro | Mali-G2 Ultra NX。 [产品规格](https://www.mediatek.com/products/smartphones/mediatek-dimensity-9600-pro) |
| 8000 / 8100 / 8200 / 8250 | Mali-G610 MC6。 [8000 / 8100](https://i.mediatek.com/dimensity-8000-series) · [8200](https://www.mediatek.com/products/smartphones/mediatek-dimensity-8200) · [8250](https://www.mediatek.com/products/smartphones/mediatek-dimensity-8250) |
| 8020 | Mali-G77 MC9。 [产品规格](https://www.mediatek.com/products/smartphones/mediatek-dimensity-8020) |
| 8300 / 8350 | Mali-G615 MC6。 [8300](https://www.mediatek.com/products/smartphones/mediatek-dimensity-8300) · [8350](https://www.mediatek.com/products/smartphones/mediatek-dimensity-8350) |
| 8400 / 8450，8500 / 8550 | 分别为 Mali-G720 MC7、MC8。 [8400](https://www.mediatek.com/products/smartphones/mediatek-dimensity-8400) · [8450](https://www.mediatek.com/products/smartphones/mediatek-dimensity-8450) · [8500](https://www.mediatek.com/products/smartphones/mediatek-dimensity-8500) · [8550](https://www.mediatek.com/products/smartphones/mediatek-dimensity-8550) |
| 7200 / 7350，7300 / 7400(X) | 分别为 Mali-G610 MC4、Mali-G615 MC2。 [7200](https://www.mediatek.com/products/smartphones/mediatek-dimensity-7200) · [7350](https://www.mediatek.com/zh-cn/products/smartphones/mediatek-dimensity-7350) · [7300](https://www.mediatek.com/zh-cn/products/smartphones/mediatek-dimensity-7300) · [7400](https://www.mediatek.com/press-room/mediatek-dimensity-7400-and-dimensity-6400-makes-improved-gaming-connectivity-and-ai-performance-more-accessible) |
| 7050，6300 / 6400，7020 / 7025 | 分别为 Mali-G68 MC4、Mali-G57 MC2、Imagination IMG BXM-8-256。 [7050](https://www.mediatek.com/products/smartphones/mediatek-dimensity-7050) · [6300](https://www.mediatek.com/products/smartphones/mediatek-dimensity-6300) · [6400](https://www.mediatek.com/press-room/mediatek-dimensity-7400-and-dimensity-6400-makes-improved-gaming-connectivity-and-ai-performance-more-accessible) · [7020](https://www.mediatek.com/products/smartphones/mediatek-dimensity-7020) · [7025](https://www.mediatek.com/products/smartphones/mediatek-dimensity-7025) |
| 1080 | Mali-G68 MC4。纳入旧机小处理尺寸与原帧回归。 [MediaTek 规格](https://i.mediatek.com/dimensity-1080) |

天玑以通用 RIFE Vulkan + AHB 为第一条验证路线，较低的处理尺寸也可单独验证。公开 G610 样本在 Vulkan 1.1.177 已报告 FP16，G615 和多份 G720/G925/G1 Ultra 样本的计算 subgroup 为 16。两份 G720 报告中的协作矩阵随驱动版本不同。分别查询 FP16 算术与 16 位存储，自定义 compute shader 按实际 subgroup 和共享内存约束选工作组。 [G610](https://vulkan.gpuinfo.org/displayreport.php?id=50966) · [G615](https://vulkan.gpuinfo.org/displayreport.php?id=53508) · [G720 44.1.0](https://vulkan.gpuinfo.org/displayreport.php?id=50964) · [G720 49.1.0](https://vulkan.gpuinfo.org/displayreport.php?id=52479) · [G925](https://vulkan.gpuinfo.org/displayreport.php?id=53119) · [Arm 工作组建议](https://support.arm.com/documentation/101897/0400/Compute-shading/Workgroup-sizes)

两份天玑 9500 公开报告均为 G1 Ultra MC12、Vulkan 1.3.305、驱动 54.1.0，并报告计算阶段的协作矩阵功能。两台手机的 `driverUUID` 与 `pipelineCacheUUID` 均不同。持久化性能记录要核对驱动身份和 workload，Vulkan 管线缓存要额外校验缓存头标识。 [OPPO 查询](https://vulkan.gpuinfo.org/displayreport.php?id=52709) · [vivo 查询](https://vulkan.gpuinfo.org/displayreport.php?id=52543) · [缓存头](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineCacheHeaderVersionOne.html)

Arm Vulkan 张量、数据图与 Mali-G2 Ultra NX 的神经加速涉及 SDK 支持的模型格式与操作。MediaTek MFRC 面向 Vulkan 游戏，官方申请要求目标设备、系统与引擎信息。两条路线均需确认接入许可和实际视频 workload。 [Arm ML SDK](https://developer.arm.com/mobile-graphics-and-gaming/ml-sdk-vulkan) · [MFRC FAQ](https://developer.mediatek.com/MFRC/695150639a8bb35dac3c0609.html) · [MFRC 申请](https://developer.mediatek.com/MFRC/695151be9a8bb35dac3c060b.html)

## 运行时选择与验收

Android Vulkan Profile 2022 适合核对 Vulkan 1.1 与 AHB 互操作基线。扩展版本、AVP 与 CTS 记录用于确定探测起点，逐资源和工作负载的资格仍由当前整机给出。 [Android Vulkan Profiles](https://developer.android.com/ndk/guides/graphics/android-vulkan-profile)

1. **按真实资源验证技术资格。** 查询 RGBA8 AHB 的格式、用法、extent 与 Vulkan 导入属性，实际验证 EGL framebuffer、Vulkan 采样和输出用途。GPU_SYNC 还需通过 Android native fence 与 Vulkan SYNC_FD 的双向探测和真实交接。HOST_WAIT 可以单独验收。 [Khronos AHB](https://docs.vulkan.org/refpages/latest/refpages/source/VK_ANDROID_external_memory_android_hardware_buffer.html) · [外部信号量属性](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceExternalSemaphoreInfo.html) · [Android 图形同步](https://source.android.com/docs/core/graphics/sync)
2. **按实际模型验证效果。** ncnn 按当前 GPU 与启用特性选 FP16、subgroup 和矩阵。矩阵需枚举算术、输入和累加类型与支持尺寸，再确认模型实际加载、输出和回收。 [Khronos 矩阵枚举](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR.html)
3. **按实测预算选择。** 对通过画质和设备验证的配置记录帧对到绘制提交 p95，资格绑定源和输出尺寸、处理尺寸、固定 flow 倍率与 AHB 同步路径。在预算内按处理像素数、flow 细度和延时选择。宿主继续记录模型、精度、优化参数与设备代际。 [Media 接入与资源归属](ADR-0004-android-content-bridge.md)
4. **按故障处置。** 路径不支持时选已验证的备用配置，准备繁忙时先呈现原帧，设备丢失时重建域。异步准备切换仍由原 owner 完成退休。发布验收覆盖长时播放、原帧连续性、画质、热稳态、拖动、切清晰度与音画同步。
