---
name: meguviz-media
description: 在 C/C++ 或 Android 消费方接入 MeguvizMedia 时间线、identity、帧调度或补帧；将单独的 RIFE/QCOM 路径迁移到统一帧生成会话，并保留播放器原帧、音频与时钟。
---

# 接入 MeguvizMedia

## 识别现有接入

找到调用方的依赖声明、JNI/DSO 加载位置、视频提交、EGL owner、时钟与 seek 路径。已有 Media 时先核对实际 API 与原生包，优先就地调整调用。使用项目已有 SDK 或对应源码 checkout 来核对下列接口。保持原有播放、时间线与回退路径可用。

| 需求 | 接入点 |
| --- | --- |
| C/C++ 时间线、帧调度或 identity | `find_package(MeguvizMedia 1.2.2 CONFIG REQUIRED)` 并链接 `MeguvizMedia::core`。参照 `examples/external_consumer`，从 `include/meguviz/media.h` 与实际用到的时间线或 cadence 头开始。 |
| Android identity | 使用已包含 Java API 的 `media-jni` AAR，调用 `MediaRuntime.loadPackagedIdentity()`。identity 可验证帧传递。 |
| Android 播放器补帧 | 普通应用使用已包含 Java API 的 `media-rife-jni` AAR，经 `MediaRuntime.loadPackaged()` 加载。已有按需 loader 时选择同批 API-only 和 native payload，再用 `MediaRuntime.loadValidatedAbsolutePath()` 或 `attachAlreadyLoaded()`。每种消费者只选一种装配方式。 |

QCOM 是单独的可选 `libmeguviz_qcom_motion.so`。需要它时按 `docs/ADR-0004-android-content-bridge.md` 构建并对所选 ABI 装配；切换后端仍保持 Media/Compute/RIFE 组合只有一个 owner。

## 迁移播放器的补帧路径

1. 核对源码/目标 AAR 是否已有 `FrameGenerationSession`。在当前 EGL owner 上按源尺寸、输出尺寸、色彩、视频/显示周期和实际预算创建 `FrameGenerationProfile`。两路适用 8 位 SDR 且显示周期短于源帧周期，QCOM 还要求 BT.709。继续用宿主时钟、音频和原帧绘制。需要任意 alpha、独立帧比较或超出单在途上限的宿主，先比对其行为与 `WindowBackend` 的固定中点契约，保持尚未覆盖的原有调用。
2. 仅采纳当前设备、格式与工作负载实际通过画质和时延检查的配置。RIFE 准备 `AndroidMediaSession` 并在 EGL owner 调用其 `probe()`，GPU sync 需两向 fence 验证。用 `qualifyRife` 记录精确处理尺寸、同步模式、flow 和 pair-to-draw p95，通过 `selectRife` 与 `openQualified` 选择。QCOM 需同一 profile 下独立确认能力、画质和预算，再调用 `qualify(Backend.QCOM_AHB, p95Ns)`。
3. `openQualified` 仅在返回 RIFE 时接管已准备的 RIFE session；返回 QCOM 或 null 由调用方继续持有/清理它。精确 RIFE 配置不匹配时会抛出参数异常，修正配置。通过 `capture` / `trySubmit` / `poll` / `draw` / `release` 串联中点，输入 source ID 和 PTS 严格递增，deadline 用 `System.nanoTime()` 时基。拒绝、超期、非推理结果或 draw backpressure 使用原帧。取消后继续 poll 直至退役。
4. Seek 或同配置下的不连续点用 `invalidate`，旧帧仍需归还。清晰度、色彩、节奏或设备变化重新准备 profile 与会话。释放所有持有帧才 close，在同一 EGL owner 上清理；close 失败则保留 owner 和 context，重试退役后再销毁。

若目标只是把 CPU 帧交给已有 Memory 消费者，使用 Memory 源码 `adapters/media` 的 `MeguvizMemoryMediaReadBridge::read_bridge`，匹配 Compute/Media/Memory SDK 和 CRT。该可选路径接收完整、就绪的 CPU READ，保留映射至最后 Memory 借用归还；Arena 需要相应独立目标。GPU 补帧继续使用原 AHB 和 fence。

## 验证并交付

- C/C++ 在 Media 根目录执行以下命令，把路径改成实际 Compute 源码位置。有匹配工具链与 CRT 的独立 Compute SDK 时，可以改用 `-DCMAKE_PREFIX_PATH=<Compute SDK 前缀>` 选择依赖。

  ```sh
  cmake --preset host-debug "-DMGM_COMPUTE_SOURCE=<Compute源码目录>" -DMGC_WITH_NCNN=OFF
  cmake --build --preset host-debug
  ctest --preset host-contracts --no-tests=error
  ```

- Android 在 Windows 的 `android` 目录执行 `./gradlew.bat :media-api:testDebugUnitTest :media-api:assembleRelease`，使用对应 JDK 和已安装的 Android SDK。现有组合包用 `python tools/mgv_integrate.py inspect-package --manifest <manifest.json> --out <新报告.json>` 检查。
- 对播放器用真实宿主连续播放、seek 和切清晰度，分别确认原帧与推理帧实际呈现、退役、音画同步。以同一时基记录呈现与音频；包检查、JVM 测试和离屏绘制各自只能证明对应范围。报告实际选择的后端、回退与测试结果。
