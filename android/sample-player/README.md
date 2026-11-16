# 独立播放器样例

此样例使用自有 MediaExtractor/MediaCodec 视频源、OES 纹理及 Media identity
效果，不依赖 bil 或 Xposed。它不提供音频同步或实际显示反馈。未知 SDR 元数据、
不支持的能力或运行错误会保留原始 OES 显示。

切换视频时，样例先取消旧解码线程，并等待它释放 codec、extractor 和 Surface。
等待超时或释放结果无法确认时，不启动新的解码器；视频选择结果会保存到有当前
EGL 上下文的渲染回调，等待旧解码器退出后继续。释放异常时当前 Activity 会拒绝
再次启动。当前 Activity 存活时，解码器 gate 持有实际 codec、extractor、Surface 和
SurfaceTexture；若 codec.release 抛错，不会继续调用 Surface.release。Java 返回值本身
不能证明底层 BufferQueue 或 GPU 已退休。启动失败时会释放尚未交给解码器的 Surface；
若该释放也失败，gate 保留 Surface 和纹理并阻止继续启动。延迟切换请求在暂停或更新请求后会失效；
释放失败、线程退出且 Activity 被回收时，当前实现
没有进程级所有者来保证 codec、Surface 和 SurfaceTexture 的持续保活。这是样例中的主机线程控制，
不代表已验证设备上的 BufferQueue 行为，也不能证明 GPU 任务已物理退役。

同一 Activity 实例返回前台时，样例会重新排队上次选择的视频，从头开始解码；若文件选择器在暂停期间
返回了新的选择，则使用新选择。恢复仍受旧解码器退出和 EGL 上下文可用性约束，尚未
验证设备上的后台恢复或播放位置保持；配置变化或进程重建后不保证恢复选择。

暂停和销毁的完整资源回收尚未解决：若取消后 500 毫秒内解码器没有退出，当前
实现会跳过这次会话重置，也不会在该线程之后退出时自动补做清理。暂停时 EGL
上下文还可能丢失，而 Media 会话只能由创建它的线程及 EGL 上下文清理。当前
样例不应作为已验证的后台恢复或长时间运行方案；文件选择器也可能触发暂停。
详细源码审查见工作区 `coordination/review/sample-decoder-pause-lifecycle-20261030/REVIEW.md`。

可在 JDK 17 下对纯 Java 交接逻辑运行针对性主机测试；以下命令从本目录执行，
将临时类文件写入单独的输出目录：

```powershell
New-Item -ItemType Directory -Force -Path build/decoder-gate-test | Out-Null
javac -d build/decoder-gate-test src/main/java/io/github/jichuo1/meguviz/media/player/DecoderGate.java src/main/java/io/github/jichuo1/meguviz/media/player/DecoderResources.java src/main/java/io/github/jichuo1/meguviz/media/player/DeferredStart.java src/main/java/io/github/jichuo1/meguviz/media/player/DeferredRetry.java src/test/java/io/github/jichuo1/meguviz/media/player/DecoderGateTest.java src/test/java/io/github/jichuo1/meguviz/media/player/DecoderResourcesTest.java src/test/java/io/github/jichuo1/meguviz/media/player/DeferredStartTest.java src/test/java/io/github/jichuo1/meguviz/media/player/DeferredRetryTest.java
java -cp build/decoder-gate-test io.github.jichuo1.meguviz.media.player.DecoderGateTest
java -cp build/decoder-gate-test io.github.jichuo1.meguviz.media.player.DecoderResourcesTest
java -cp build/decoder-gate-test io.github.jichuo1.meguviz.media.player.DeferredStartTest
java -cp build/decoder-gate-test io.github.jichuo1.meguviz.media.player.DeferredRetryTest
```

RIFE 接入候选仍需底层 GPU 错误后资源退役的可靠合同及设备验证，尚未并入此样例。
