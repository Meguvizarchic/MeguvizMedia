# MeguvizMedia 迁移对照与排障

本文档梳理从独立 RIFE / QCOM 或旧版 `AndroidMediaSession` 迁移到 `FrameGenerationSession` 的代码范式、所有权与线程铁律、高频状态排查表以及安全回退方案。

---

## 1. 存量代码迁移对照范式

### 场景 A：从直连 `AndroidMediaSession` / 独立 QCOM 迁移到 `FrameGenerationSession`

- **旧版做法**：播放器中同时写两套代码——分别判断是否加载 `libmeguviz_qcom_motion.so`、手写 `GlesMotionWarp` 生命周期，以及在另一条分支里直接维护 `AndroidMediaSession` 的 `submit` / `pollCompleted` / `poll` 状态机，且经常出现 RIFE 打开失败后忘记关闭准备好的 session 或混用线程。
- **推荐写法**：统一收敛为 `FrameGenerationProfile` + `openQualified`（或旧版 `open`），由 `FrameGenerationSession` 统一接管采集、提交、轮询、绘制与代际管理。

```java
// 1. 准备工作：创建 Profile 与候选配置
FrameGenerationProfile profile = new FrameGenerationProfile(
    w, h, outW, outH, primaries, matrix, true,
    sourcePeriodNs, displayPeriodNs, budgetNs);

// 2. 分别登记已完成设备与画质验证的 RIFE / QCOM 配置
FrameGenerationProfile.Qualification rife1080 =
    profile.qualifyRife(AndroidMediaSession.AHB_HOST_WAIT, 4, p95_1080);
FrameGenerationProfile.Qualification rife720 =
    profile.qualifyRife(1280, 720, AndroidMediaSession.AHB_HOST_WAIT, 4, p95_720);
FrameGenerationProfile.Qualification bestRife = profile.selectRife(rife1080, rife720);

FrameGenerationProfile.Qualification qcom =
    profile.qualify(FrameGenerationProfile.Backend.QCOM_AHB, p95_qcom);

// 3. 在 EGL owner 上对准备好的 RIFE 执行 probe() 后再交由 openQualified 选择
if (preparedRife != null) {
    long[] probe = preparedRife.probe();
    // 确认 probe 成功后再打开
}
FrameGenerationSession session = FrameGenerationSession.openQualified(profile, bestRife, preparedRife, qcom);

// 4. 所有权检查：未选中 RIFE 时必须释放传入的 preparedRife
if (preparedRife != null && (session == null || session.backend() != FrameGenerationProfile.Backend.RIFE)) {
    preparedRife.close();
}
if (session == null) {
    // 两条补帧路径均不满足时，直接走宿主原帧呈现逻辑
}
```

### 场景 B：从 `submit()` 迁移到 `trySubmit()` 与原帧回退

- **旧版做法**：调用 `session.submit(...)` 只返回 `0` 或正数，无法区分当前是因为上一帧还在运算、剩余时间不够还是输入未就绪而拒绝。
- **推荐写法**：改用 `session.trySubmit(left, right, deadlineNs)`，并在以下任意情况直接切回宿主原帧绘制：
  - `Submission.decision == Decision.IN_FLIGHT`（已有任务在途，最多同时处理 1 个请求）；
  - `Decision.DEADLINE`（按登记的 `pairToDrawP95Ns` 估算已超过 `deadlineNs`）；
  - `Decision.INPUT_NOT_READY`（如紧凑亮度读回尚未就绪）；
  - `Decision.BACKEND_REJECTED`；
  - `poll()` 返回结果中 `status != 0` 或 `frame == null`；
  - `draw()` 抛出 `WindowBackend.Backpressure`。

### 场景 C：异步准备与安全取消 (`AndroidMediaSession.Preparation`)

创建 RIFE 会话可能触发较耗时的计划建立和 Vulkan 导入，建议使用 `AndroidMediaSession.newPreparation(...)` 分阶段推进，避免阻塞 GL 渲染线程：

1. 后台线程调用 `prep.start()`。
2. 状态达到 `READY_TO_BEGIN` 后，切回 EGL owner 线程调用 `prep.beginOnGl()`（不阻塞）。
3. 后台工作线程自动推进导入至 `READY_TO_COMMIT` 后，回到同一个 EGL owner 调用 `prep.commitOnGl()` 获取 `AndroidMediaSession`。
4. **若中途放弃或取消**：调用 `prep.cancel()` 只是发出取消信号，**严禁立即销毁上层的引擎句柄或卸载原生库**，必须持续轮询直到 `prep.retirementStatus() == 0` 且同步清理 GL 线程上的 `servicePendingCreation()`，确认所有后台任务和 GL 资源都归还后才算结束。

### 场景 D：CPU 帧桥接到 `MeguvizMemory`（仅在需要时）

- 只有当进程内已有 `MeguvizMemory` 消费者需要直接读取 Media 产出的完整、就绪 CPU `READ` 帧时，才引入 Memory 仓库 `adapters/media` 的 `MeguvizMemoryMediaReadBridge::read_bridge`（需保证 Compute/Media/Memory 工具链与 CRT 一致；使用 Arena 时需额外链接对应目标）。
- 桥接期间需保持 CPU 映射有效，直到 Memory 侧最后一个借用归还。Android 视频播放器的 GPU OES/AHB 补帧不得混入 CPU 桥接，继续沿用原 AHB 与 fence 路径。

---

## 2. 生命周期、线程与所有权铁律

1. **同一 EGL owner 线程铁律**：`FrameGenerationSession` 和 `AndroidMediaSession` 的捕获、提交、轮询、绘制、帧释放与 `close()` 都必须在创建实例的 EGL owner 线程中完成，且调用时该窗口的 EGL 上下文处于 current 状态。
2. **纹理矩阵不重复翻转**：`AndroidMediaSession` 内部在采集时对原始 `SurfaceTexture.getTransformMatrix(...)` 应用了垂直翻转，绘制时再翻回；调用方传入原始矩阵即可，不要在外部再额外做一次垂直翻转。
3. **帧引用配对**：每次 `capture()` 成功的返回值与每次 `poll()` 返回的有效 `Completion.frame` 均拥有一份独立帧句柄，使用完毕后（哪怕因跳帧未绘制或执行了 `invalidate()`）都必须在 owner 线程调用一次 `session.release(frame)`。
4. **`invalidate()` 与切清晰度**：Seek、倍速跳变或时间戳不连续时调用 `invalidate()` 递增代际；旧代际的帧不能再传给 `trySubmit` 或 `draw`（会抛出 `stale frame generation`），仍须显式 `release()`；已入队的在途任务须继续 `poll()` 至收到终态回执。切分辨率、色彩空间、帧率或重置表面时，先排空并关闭当前会话，再建新 `FrameGenerationProfile` 与新会话。
5. **关闭前必先归还全部帧**：`FrameGenerationSession.close()` 会检查 `ownedFrames == 0`；如果仍有未 `release()` 的帧，会直接抛出 `IllegalStateException("release frame references before close")`。若后端关闭时因异步资源尚未归还而抛出 `WindowBackend.Backpressure` 或 `MediaException(-7)`，**绝对不能销毁 EGL 上下文或退出 owner 线程**，必须保留上下文并在后续渲染节拍继续调用 `close()` 重试直到成功。

---

## 3. 高频异常与状态码排查表

| 现象 / 异常 / 状态码 | 常见根因 | 解决与核对方式 |
| --- | --- | --- |
| `IllegalArgumentException: prepared RIFE session does not match its qualified AHB configuration` | 传给 `openQualified` 的 `preparedRife` 未先在 EGL owner 调用 `probe()`、`probe()` 失败，或创建 session 时的处理宽高/同步路径与 `Qualification` 不一致 | 创建后先在 EGL owner 调 `probe()` 检查成功（`GPU_SYNC` 需双向 fence 均返回 0），再核对 `width/height/sourceWidth/sourceHeight/path` 与 `qualifyRife` 参数一致 |
| `IllegalArgumentException: invalid source pair` | `trySubmit(left, right, ...)` 的 `left.sourceId >= right.sourceId` 或 `left.ptsUs >= right.ptsUs` | 确保 `capture()` 传入单调递增的 `sourceId` 与 `ptsUs`；PTS 回跳时先调用 `invalidate()` |
| `IllegalStateException: use the creating EGL owner` | 在后台解码线程或 UI 线程调用了 `FrameGenerationSession` / `AndroidMediaSession` | 将所有接口调用全部派发回创建会话的 GL 线程 |
| `IllegalStateException: release frame references before close` | 调用 `close()` 时仍有通过 `capture()` 或 `poll()` 获取但未 `release()` 的帧 | 用显式引用集合跟踪所有打开的帧，关闭前全部调用 `session.release(frame)` |
| `WindowBackend.Backpressure` (`draw` 或 `close`) | `draw()` 时输出已超出 `deadlineNs` 或底层渲染繁忙；`close()` 时仍有 GPU/后台读工作未结束 | `draw()` 遇此异常直接改绘宿主原帧；`close()` 遇此异常保留 EGL 上下文稍后重试 |
| Native `-7` (`MGC_BUSY`) | 尚有消费锁、GL 读依赖或后台关闭队列未完成 | 继续调用 `poll()` 推进回收，或从后台服务线程推进 `MediaRetirementService` 后重试 |
| Native `-8` (`MGC_BACKPRESSURE`) | 捕获或输出池槽位已满 | 检查是否有帧忘记 `release()`，或增大 `poolCapacity`（3–12） |
| Native `-11` (`MGC_DEVICE_LOST`) | GPU 驱动发生设备丢失 | 停止提交新任务，释放持有帧并关闭旧会话（底层通过后台服务回收）；切回原帧并在新设备代际重建 |
| Native `-13` (`MGC_CANCELLED`) | 会话处于暂停态（`paused` 或宿主 `clock.paused`）或任务已被取消 | 检查暂停状态，跳过补帧并呈现当前原帧 |

---

## 4. 安全回退方案

1. **初始化/探测阶段回退**：当设备非 8 位 SDR、`displayPeriodNs >= sourcePeriodNs`、`openQualified` 返回 `null`、或 RIFE `probe()` 与 QCOM 加载均不可用时，不创建补帧会话，直接以零开销运行宿主原有 OES 解码显示路径。
2. **运行时受控关闭与切回**：需要随时降级回原帧或旧 ByteBuffer 路径时，按以下顺序操作：停止调用 `capture` / `trySubmit` → 调用 `invalidate()`（或提升 epoch）→ 循环 `poll()` 直至在途任务收到终态回执 → `release()` 所有持有帧 → 在 EGL owner 上调用 `close()` 重试直至成功 → 切回宿主原 OES/旧路径（不要强行销毁仍在被 GPU 读取的 Surface 或 EGL 上下文）。
