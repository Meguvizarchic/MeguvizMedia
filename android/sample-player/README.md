# 独立播放器样例

此样例使用独立的 `MediaExtractor` / `MediaCodec` 视频源、OES 纹理及 MeguvizMedia Identity 传递效果，不依赖第三方播放宿主或 Xposed 框架，同时不包含音频同步与实际显示完成反馈。遇到未知 SDR 元数据、不支持的设备能力或运行错误时，会自动保留原始 OES 直接显示。

## 视频切换、生命周期与已知局限

1. **解码器停止与延迟切换**：切换视频时，样例会先取消旧解码线程并等待其释放 `MediaCodec`、`MediaExtractor` 与 `Surface`；若等待超时或释放结果无法确认，则阻止启动新解码器。新的选片请求会暂存到持有当前 EGL 上下文的渲染回调中，待旧解码线程退出后再继续执行；若释放出现异常，当前 Activity 会拒绝再次启动。
2. **局部对象所有权**：在当前 Activity 存活期间，`DecoderGate` 负责持有实际的 `MediaCodec`、`MediaExtractor`、`Surface` 与 `SurfaceTexture`。若 `codec.release()` 抛出异常，代码不会继续执行 `Surface.release()`；启动失败时会释放尚未交付给解码器的 `Surface`，若该释放也失败则会保留相关对象并阻止后续重启。延迟切换请求在暂停或收到新请求后会作废。此外，若释放失败且线程退出后 Activity 被回收，当前样例还没有进程级托管者来持续保活这些对象。
3. **前后台恢复限制**：同一个 Activity 实例切回前台时，样例会重新将上次选择的视频排入队列并从头开始解码（若文件选择器在暂停期间返回了新视频，则改用新选择）。恢复过程仍受旧解码器退出状态和 EGL 上下文可用性制约，配置变更或进程重建后不保证恢复选片。
4. **暂停与销毁阶段的已知未完项**：若取消后 500 毫秒内旧解码器未能退出，当前实现会跳过该次会话重置，也不会在线程随后退出时自动补做清理。暂停期间 EGL 上下文仍可能丢失，而 Media 会话只能由创建它的线程及 EGL 上下文清理（文件选择器也可能触发暂停）。因此，此样例不宜作为后台恢复或长时稳态方案参考；详细源码审查记录见工作区 `coordination/review/sample-decoder-pause-lifecycle-20261030/REVIEW.md`。

## 纯 Java 交接逻辑主机测试

在 JDK 17 环境下，可直接从本目录对纯 Java 解码门控逻辑运行以下单测（会将临时 `.class` 输出到独立的 `build/decoder-gate-test` 目录）：

```powershell
New-Item -ItemType Directory -Force -Path build/decoder-gate-test | Out-Null
javac -d build/decoder-gate-test src/main/java/io/github/jichuo1/meguviz/media/player/DecoderGate.java src/main/java/io/github/jichuo1/meguviz/media/player/DecoderResources.java src/main/java/io/github/jichuo1/meguviz/media/player/DeferredStart.java src/main/java/io/github/jichuo1/meguviz/media/player/DeferredRetry.java src/test/java/io/github/jichuo1/meguviz/media/player/DecoderGateTest.java src/test/java/io/github/jichuo1/meguviz/media/player/DecoderResourcesTest.java src/test/java/io/github/jichuo1/meguviz/media/player/DeferredStartTest.java src/test/java/io/github/jichuo1/meguviz/media/player/DeferredRetryTest.java
java -cp build/decoder-gate-test io.github.jichuo1.meguviz.media.player.DecoderGateTest
java -cp build/decoder-gate-test io.github.jichuo1.meguviz.media.player.DecoderResourcesTest
java -cp build/decoder-gate-test io.github.jichuo1.meguviz.media.player.DeferredStartTest
java -cp build/decoder-gate-test io.github.jichuo1.meguviz.media.player.DeferredRetryTest
```

## 适用范围与验证说明

本样例中的门控与线程切换仅用于验证宿主层并发控制，Java 接口的返回并不能证明底层 `BufferQueue` 或 GPU 任务已经彻底结束并归还资源，也不代表真机上的长时恢复与性能结果；此外，RIFE 补帧链路仍待补充底层 GPU 报错后的资源回收合同与真机验收，尚未合入本样例。
