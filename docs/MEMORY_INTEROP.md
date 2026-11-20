# 与 MeguvizMemory 共用 CPU 帧

已有解码、图像处理或读取任务将 CPU 帧写入 MeguvizMemory 时，可以把同一块内存借给 Compute，再由 Media 管理时间线或由 RIFE 插值。CPU 导入和 Media identity 共用原始像素。RIFE 的像素预处理、推理和输出池仍有各自的内存成本。

## 选一个接入方向

| 现有数据路径 | 接入方式 |
| --- | --- |
| Memory 已拥有 CPU 像素，Media 或 RIFE 要读取 | 用 Memory 的可选 `MeguvizMemoryComputeHostBridge::host_bridge` 创建 Compute host resource。从完整且就绪的内容取得 Compute READ lease。 |
| Compute 或 Media 已拥有完整 CPU READ，Memory 要读取 | 选 Memory 的 `adapters/compute` 或 `adapters/media` 只读桥。借用期间保留映射，处理完成后及时归还。Arena 需要相应的可选目标。 |
| 同一份不变的数据重复上传，或多次下载到同一块 CPU 内存 | 用 Memory `adapters/compute_host` 的准备和重复提交接口。下载完成后通过该阶段的 map/unmap 读取。上传内容会变时重新准备或用一次性路径。 |
| 输入已经是 GPU-only、外部纹理或 AHardwareBuffer | 沿用 Compute/Media 的设备归属、格式探测和 acquire/release fence。CPU 源写入 GPU-only AHB 时可评估 Memory 的独立 GPU/AHB 适配器，按实际设备验证同步和成本。 |

## 把一帧从 Memory 交给 Media 或 RIFE

1. 用同一套工具链、ABI 和 C++ 运行库构建 SDK 与可选桥。确认 Compute SDK 导出 `mgc_resource_import_host_v1`。保留现有 SDK 的原路径作为回退。
2. 完成 CPU 写入并归还写借用。用 `mgmem::compute::host`（C 中为 `mgmem_compute_host`）以 READ 模式导入，配好 CPU 格式、宽高和实际行距。从返回的 `MgcResource` 创建 READ lease 后，可归还资源的本地引用。错误回调和 Memory domain 继续存活。
3. 用该 lease 填 `MgmFrameDesc.content`，设置一致的帧布局、PTS 和 epoch。把帧交给 identity effect 时显式准备提交引用。原生 CPU RIFE 也可直接消费同 Compute domain 的两份 RGB8 READ lease，填入 `MgrRequest.left/right`，输出按 RIFE 的结果合同归还。使用 `mgm_frame_describe` 得到的是帧持有的内容引用，跨越帧生命周期或提交给 RIFE 前先另取引用。
4. 成功提交后由接收方持有相应引用。拒绝时归还自己准备的引用。取消或超时后继续等待已接受任务退役，释放仍在消费者手中的帧与 lease，再结束相关 Memory domain。切清晰度或 seek 后用新 epoch 接受新帧，旧帧完成退役后再归还。

只有调用方原本就需要把同一批 CPU 像素复制到 Compute 时，以上路径才能直接省去那次复制。两个库都可能对同一 backing 计入各自的准入预算，测量物理内存时应避免把这些逻辑计数相加。

用相同尺寸、行距、PTS 和输入比对输出，核对早释放上游 owner 后的读取、提交拒绝、取消和最后一次归还。CPU 上的逐字节等价和耗时用于评估 CPU 边界。呈现、音画同步、Android AHB 和功耗需要各自在真实宿主或设备上验证。

[Memory 主机资源桥](https://github.com/Meguvizarchic/MeguvizMemory/tree/main/adapters/compute_host) · [Memory 帧读取桥](https://github.com/Meguvizarchic/MeguvizMemory/tree/main/adapters/media) · [Media 接入技能](../.agents/skills/meguviz-media/SKILL.md)
