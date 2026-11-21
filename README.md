<p align="center"><img src="docs/brand/MeguvizMedia.svg" width="96" alt="MeguvizMedia"></p>

# MeguvizMedia

面向 Android 播放器与 C/C++ 媒体处理的帧时间线、呈现调度和插帧库。

- 提供帧封装、播放时间线与有界调度接口，支持按播放进度安全处理 Seek 与分辨率变化。
- Android 播放器在满足设备能力、8 位 SDR 色彩和时延预算条件时，可选用 RIFE 或 Qualcomm 运动估计后端进行中点补帧，同时由宿主继续管理音频、时钟与原帧呈现。
- C/C++ 应用可独立接入核心帧处理与时间线能力，并按需桥接 RIFE 或 MeguvizMemory。

## Android 接入

Android 组合模块要求 API 27+。常规应用可直接从源码构建 `media-rife-jni` AAR，并在使用前调用 `MediaRuntime.loadPackaged()`。带有独立动态加载器的宿主应用，可使用同批构建的纯 Java `media-api` AAR 与 `native-payload` 原生库。每个消费方只能选择一种装配方式。

源码构建顺序：先生成 `:media-api:assembleRelease`，以该 AAR 作为输入运行仓库的组合打包工具，再通过 `-Pmeguviz.compositionDir` 构建 `:media-rife-jni:assembleRelease`。[Android 接入与构建说明](docs/ADR-0004-android-content-bridge.md) · [组合构建脚本](tools/build_android_composition.py) · [独立播放器示例](android/sample-player/README.md)

## C/C++ 接入

将安装目录加入 `CMAKE_PREFIX_PATH` 后链接核心库：

```cmake
find_package(MeguvizMedia 1.2.2 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE MeguvizMedia::core)
```

需要结合 RIFE 插帧时，在构建期启用 `-DMGM_WITH_RIFE=ON` 并链接 `MeguvizMedia::rife_integration`。[公共 C 接口](include/meguviz/media.h) · [Memory CPU 帧接入](docs/MEMORY_INTEROP.md)

## 文档

- [Android GPU 兼容性索引](docs/ANDROID_GPU_COMPATIBILITY.md)
- [其他平台 GPU 适配说明](docs/GPU_PLATFORM_COMPATIBILITY.md)
- [模型上传失败与延迟关闭机制](docs/MODEL_UPLOAD_CLOSURE.md)
- [模拟时间线与截止时间回放](docs/TRACE_REPLAY.md)
- [AI 接入与迁移指南](.agents/skills/meguviz-media/SKILL.md)

[GPL-3.0 许可证](LICENSE) · [第三方声明](NOTICE.md)
