# 与 MeguvizMemory 共用 CPU 帧

当已有解码、图像处理或读取任务把 CPU 帧写入 MeguvizMemory 时，可以把同一块内存直接借给 MeguvizCompute，再交由 MeguvizMedia 管理时间线或交由 MeguvizRIFE 执行插帧。CPU 内存导入与 Media Identity 传递均共用原始像素；RIFE 的像素预处理、推理计算和输出池仍会有各自的内存占用。

## 选择桥接方向

| 现有数据路径 | 推荐接入方式 |
| --- | --- |
| Memory 已拥有 CPU 像素，Media 或 RIFE 要读取 | 使用 Memory 可选的 `MeguvizMemoryComputeHostBridge::host_bridge` 创建 Compute 宿主资源，并从完整且就绪的内容获取 Compute `READ` 租约。 |
| Compute 或 Media 已拥有完整 CPU `READ`，Memory 要读取 | 选用 Memory 的 `adapters/compute` 或 `adapters/media` 只读桥（`MeguvizMemoryMediaReadBridge::read_bridge`）。借用期间须保留内存映射，处理完毕后及时归还；使用 Arena 时需链接对应的可选目标。 |
| 同一份不变的数据反复上传，或多次下载到同一块 CPU 内存 | 使用 Memory `adapters/compute_host` 的预备与重复提交接口。下载完成后通过该阶段的 map/unmap 读取；若上传内容会变化，应重新预备或改用一次性路径。 |
| 输入已经是 GPU 专用缓冲、外部纹理或 `AHardwareBuffer` | 直接沿用 Compute/Media 的设备归属、格式探测和 acquire/release fence；CPU 源写入 GPU 专用 AHB 时可评估 Memory 的独立 GPU/AHB 适配器并按真机实测同步与开销。 |

## 将 CPU 帧从 Memory 交给 Media 或 RIFE

1. **统一构建环境**：使用同一套工具链、ABI 和 C++ 运行库构建各 SDK 与可选桥，确认 Compute SDK 已导出 `mgc_resource_import_host_v1`，并保留旧有路径作为回退方案。
2. **完成写入并获取租约**：待 CPU 写入全部完成并归还写借用后，用 `mgmem::compute::host`（C 接口为 `mgmem_compute_host`）以 `READ` 模式导入，配置一致的像素格式、宽高和行跨度；从返回的 `MgcResource` 创建 `READ` 租约后即可释放本地资源引用，同时保持错误回调与 Memory 域存活。
3. **传递给 Media 或 RIFE**：将该租约填入 `MgmFrameDesc.content` 并设置匹配的帧布局、PTS 与 `epoch`；交给 Identity 效果时显式准备提交引用。原生 CPU RIFE 也可直接消费同 Compute 域下的两份 `RGB8` `READ` 租约（填入 `MgrRequest.left` / `right`），输出按 RIFE 结果合同释放。注意 `mgm_frame_describe` 返回的是帧内部持有的内容引用，若需跨越帧寿命或提交给 RIFE，请先额外 `retain`。
4. **生命周期收尾**：成功提交后由接收方持有相应引用，拒绝时由调用方归还自备引用；取消或超时后等待已接受任务彻底结束，释放消费者手中的所有帧与租约，最后再关闭相关 Memory 域。切换清晰度或 Seek 后通过新 `epoch` 接收新帧，旧帧完成回收后再行归还。

## 适用范围与验证说明

只有当应用原本就需要把这批 CPU 像素拷贝进 Compute 时，上述桥接才能够免去此次内存拷贝。由于两个库都可能把同一块底层内存计入各自的内存配额，统计物理内存占用时应避免简单相加逻辑配额。请在相同尺寸、行跨度、PTS 和输入下比对输出字节与耗时，验证上游提前释放、提交拒绝、取消和最终归还边界；真实播放呈现、音画同步、Android AHB 与功耗仍须在目标宿主和设备环境下单独验证。

[Memory 主机资源桥](https://github.com/Meguvizarchic/MeguvizMemory/tree/main/adapters/compute_host) · [Memory 帧读取桥](https://github.com/Meguvizarchic/MeguvizMemory/tree/main/adapters/media) · [AI 接入与迁移指南](../.agents/skills/meguviz-media/SKILL.md)
