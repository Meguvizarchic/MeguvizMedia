# 模拟时间线与截止时间回放

`mgm_trace_replay --events INPUT --out OUTPUT` 用于通过 Media 时间线规划器（planner）与调度器（admission）接口，回放**模拟构造的宿主场景**。

`seek_speed.jsonl` 覆盖了 `1x` 到 `2x` 倍速切换以及 Seek 跳转场景（两者均需要开启新的 `epoch`），同时原生的无参数与 `--delayed` 时间线示例依然保留：

```text
mgm_trace_replay --events tests/fixtures/timeline/deadline_pressure.jsonl --out decisions.jsonl
mgm_trace_replay --events tests/fixtures/timeline/seek_speed.jsonl --out seek-speed-decisions.jsonl
```

## 与 Kotlin 原始参考实现的对比验证

`tests/test_seek_speed_parity.py` 会将上述测试夹具与原始 BIL Kotlin 时间线逐行对照，核对倍速切换和 Seek 期间的待处理提案一致性：

- 通过 `--exe <mgm_trace_replay>` 与 `--out-dir <new-directory>` 指定可执行文件及新输出目录；在 Windows 上如需覆盖本地默认路径，可附加 `--legacy-repo <BIL-repository>`、`--gradle-cache <modules-2/files-2.1>` 及 `--java <java-executable>`。
- 脚本运行前会核验冻结的 M0 源码哈希与提交内容，并要求已具备 Kotlin `2.4.21` 编译及运行 jar 包。
- 缺少或无效的输入路径会在创建输出目录前直接报错；预检通过后，每次启动进程前都会先把命令行参数记入 `commands.json`（含 `git show` 查询），若启动失败则在新目录保留 `commands.json`、失败阶段与标准错误日志。工具不会把外部可执行文件当作探测探针试跑。

## 输入文件规范与防覆盖保护

- **输出防覆盖**：指定输出文件在运行前绝不能已存在；工具只有在整个输入文件全部校验并回放成功后才独占创建输出文件（仅当写入本身出错时可能留下部分内容），绝不会替换已有文件。
- **输入规模与 JSON 语法**：输入须为不超过 8 MiB 的常规非符号链接文件，最多 32,768 行、每行不超过 8,192 字节，输出上限 16 MiB。每一行必须为扁平 JSON 对象：键名唯一且仅含 ASCII 小写字母、数字与下划线，值均为有符号 64 位整数（字符串仅允许标识符；禁止转义字符、浮点数、布尔值、`null`、数组和嵌套对象）。
- **行序规则**：每行必须包含从 `1` 开始连续递增的 `seq` 与操作类型 `op`，拒绝缺失或未知字段；第一行且仅第一行必须为 `config`。

## 操作指令一览

`config` 行必须提供：`target_fps`、`frame_window`、`max_clock_age_ns`、`presentation_delay_ns`、`max_lead_ns`、`min_lead_ns`、`original_tolerance_us`、`max_clock_uncertainty_us`、`phase_lock`、`cold_cost_ns`、`safety_margin_ns`、`sample_window`、`percentile`、`max_in_flight` 与 `enabled`（完整示例可参阅内置夹具）。其余指令如下：

| 操作 (`op`) | 必填字段 | 语义说明 |
| --- | --- | --- |
| `begin` | `media`, `decoder`, `surface`, `format`, `discontinuity` | 开启新 `epoch`，输出规划器代际编号与丢弃的帧 ID |
| `clock` | `media_us`, `monotonic_ns`, `speed_milli`, `discontinuity`, `uncertainty_us`, `paused` | 更新时钟快照（倍速为整数 `speed_milli` 除以 `1000`） |
| `offer` | `frame_id`, `pts_us` | 投递源帧并记录接收或丢弃的帧 ID |
| `plan` | `now_ns` | 获取规划器提案或等待原因 |
| `commit` | `now_ns` | 提交最近一次提案并报告接口返回状态 |
| `reserve` | `job_id`, `now_ns`, `deadline_ns` | 在活跃 `epoch` 中请求入队许可，记录决策状态、预测耗时、在途计数与样本数 |
| `cancel` | `job_id` | 标记模拟任务抑制输出发布（其入队槽位仍保持占用直到结束） |
| `retire` | `job_id`, `joint_ns`, `succeeded` | 声明模拟的底层任务已结束并归还槽位（被取消的任务须传 `succeeded=0`） |
| `counters` | 无 | 读取规划器累计计数器 |
| `close` | 无 | 关闭当前 `epoch`，报告丢弃的帧与未完成任务状态 |

- **约束说明**：`reserve` 要求存在已通过 `begin` 开启且尚未被 `close` 或被时钟重启要求作废的 `epoch`（不提供独立无规划器的纯调度模式）。已接收的任务即使在其所属 `epoch` 关闭或要求重启后，仍可正常取消并完成结束归还。同一个模拟场景中不允许复用已接受过的 `job_id`，且文件结束前所有已接受任务必须完成 `retire` 并关闭 `epoch`，否则整体报错且不生成输出文件。

## 适用范围与验证说明

`mgm_trace_replay` 属于纯宿主逻辑回放工具：输出首行标明 `SYNTHETIC_PLANNER_REPLAY`，其时长、结束时机与状态结果均源自测试脚本中的设定输入，并非真实设备事件读取、运行时性能实测、GPU 完成证明或屏幕实际显示证明。
