<p align="center"><img src="docs/brand/MeguvizMedia.svg" width="96" alt="MeguvizMedia"></p>

# MeguvizMedia

用于 Android 播放器和 C/C++ 媒体处理的帧时间线、呈现调度与插帧库。

- 提供帧、播放时间线和有界调度接口，支持按播放位置处理跳转和格式变化。
- Android 播放器可在满足设备、颜色和时延条件时选用 RIFE 或 Qualcomm 插帧路径，并继续管理时钟、音频和原帧呈现。
- C/C++ 应用可独立使用核心帧处理，并按需接入 RIFE 或 MeguvizMemory。

## Android 接入

Android 组合要求 API 27+。普通应用可从源码构建 `media-rife-jni` AAR，并在使用前调用 `MediaRuntime.loadPackaged()`。有独立加载器的应用，使用同一组合的 API 与原生库。每个应用选择一种装配方式。

Android 源码构建先生成 `:media-api:assembleRelease`，以生成的 API AAR 运行仓库的组合工具，再使用 `-Pmeguviz.compositionDir` 构建 `:media-rife-jni:assembleRelease`。[Android 接入与构建](docs/ADR-0004-android-content-bridge.md) · [组合构建工具](tools/build_android_composition.py) · [视频播放示例](android/sample-player/)

## C/C++ 接入

将安装目录加入 `CMAKE_PREFIX_PATH` 后链接核心库：

```cmake
find_package(MeguvizMedia 1.2.2 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE MeguvizMedia::core)
```

需要插帧时，构建时启用 `MGM_WITH_RIFE` 并使用 `MeguvizMedia::rife_integration`。[公共 C 接口](include/meguviz/media.h) · [Memory CPU 帧接入](docs/MEMORY_INTEROP.md)

## 文档

- [Android GPU 兼容性](docs/ANDROID_GPU_COMPATIBILITY.md)
- [其他平台的 GPU 说明](docs/GPU_PLATFORM_COMPATIBILITY.md)
- [Codex 接入与迁移](.agents/skills/meguviz-media/SKILL.md)

[GPL-3.0 许可证](LICENSE) · [第三方声明](NOTICE.md)
