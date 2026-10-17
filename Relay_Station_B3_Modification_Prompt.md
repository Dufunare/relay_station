# TouchVLA 中继程序 ACT 闭环软件任务 Prompt

你正在修改 TouchVLA 项目的中继电脑 Qt/C++ 程序。请直接在中继电脑现场的完整 `test1` 工程中完成中继侧为了 ACT 闭环能够脱离真实硬件实施的全部软件工作，完成自动化测试、构建、分步 Git 提交和状态文档。不要只做分析，不要等待用户逐项批准；除非缺少关键源码、工程无法打开，或者已有用户修改与任务文件直接冲突，否则持续工作到所有纯软件事项完成。

## 1. 目标和系统边界

TouchVLA 的固定结构是：

```text
CR3、相机和力数据
        ↕
     中继电脑
        ↕
Windows Qt/C++ 上位机
        ↕
Linux GPU PolicyServer
```

各端职责必须保持：

- 中继负责连接 CR3 的 29999、30003、30004，转发管理/运动命令，校验并转发机器人状态、力、姿态和视频；
- Windows 上位机负责 Touch、模式管理、动作安全、动作有效期、工作空间和真机执行授权；
- Linux 只返回模型动作建议，不能直接连接中继或 CR3；
- 中继不得自行生成模型动作，不承担 ACT 训练或推理，不绕过 Windows 安全层；
- 中继不得自动 `ClearError()`、自动 `EnableRobot()`、重放旧动作或在重连后继续旧动作；
- 当前第一阶段继续兼容低速 `MovL`，只准备未来 ServoP 所需的软件转发能力，不进行 ServoP 真机控制。

Windows 上位机已经具备：

- 旧 TCP 8888 管理/运动命令客户端；
- 旧 UDP 8889 Twin 状态接收；
- 旧 UDP 8887 Force 接收；
- UDP 8886 视频接收；
- TCP 8891 v2 可靠状态客户端 `ReliableRelayClient`；
- `RobotStateStore`、`ModeManager`、`SafetySupervisor`、`ActionExecutor`；
- PolicyClient、固定零动作 mock 和无物理发送路径的 Shadow；
- 当前 Auto、Record 强制关闭，夹爪和绝对工作空间未配置。

本任务只完成中继侧软件，不修改 Windows 上位机，不连接 Linux 模型直接控制机器人。

## 2. Git 和文件保护要求

开始前执行只读检查：

1. 找到现场实际使用的 `test1` 源码、解决方案、项目文件和构建配置；
2. 查看 Git 当前分支、状态和最近提交；
3. 尽量核对当前运行 exe 对应的源码和构建配置；无法确认时记录事实，但不阻塞纯软件工作；
4. 记录现有 8888、8891、29999、30003、30004、8886、8887、8889、8890 的实现位置；
5. 记录现有线程、Socket、定时器、接收缓存和停止流程。

所有修改必须只发生在分支：

```text
vla_yj_relay
```

规则：

- 如果 `vla_yj_relay` 不存在，从现场当前可运行基线创建；
- 如果已经存在，确认它的基线正确后继续使用；
- 不在 master、main、recovery 或其他历史分支修改、提交、合并、变基或删除；
- 每次提交前再次确认当前分支是 `vla_yj_relay`；
- 保留用户已有和无关修改，不自动清理、丢弃、stash 或覆盖；
- 只暂存当前步骤涉及的文件；
- 不提交 `.vs`、构建目录、exe、pdb、临时日志、视频、图片或现场数据；
- 不自动 push，不创建远程 PR，只在本地分步提交；
- 如果已有修改与本任务直接重叠，先审查并尽量兼容；不能安全处理时，在最终报告明确列为阻塞项。

## 3. 先梳理并记录现有中继执行流

在修改前查清以下事实，并将结论写入最终状态文档：

1. 8888 TCP Server 在哪里监听；
2. 8888 如何读取、缓存和识别 `29999|...`、`30003|...`；
3. 29999 和 30003 命令由哪个对象、线程和 Socket 转发；
4. CR3 的 29999、30003 回复在哪里接收；
5. 30004 在哪里建立连接、读取和切包；
6. `DobotRealTimeData` 或等价结构在哪里定义；
7. 30004 当前是否校验 `MessageSize` 和 `TestValue`；
8. Twin、Force、Video 从哪些状态生成，使用什么端口和格式；
9. 8888 客户端断开、中继 Stop、程序关闭时的资源清理顺序；
10. 端口连接灯与机器人真正 Ready 当前怎样判断；
11. 是否有旧回复、旧 UDP 包、旧状态或旧动作跨连接复用的可能；
12. 当前是否存在 ServoP 发送后错误等待普通控制器回复的路径。

审查完成后继续实现，不要只交付审查报告。

## 4. 保留并加固旧 TCP 8888

必须继续兼容当前 Windows 上位机发送的旧格式：

```text
29999|RobotMode()
29999|EnableRobot(...)
29999|CP(0)
29999|GetPose()
29999|DisableRobot()
30003|MovL(...)
```

不能要求现有上位机立刻增加换行符或长度头。为每个 8888 客户端建立独立、有上限的接收缓存：

1. 一条命令被拆成多次 `readyRead` 时，半条命令留在缓存等待；
2. 多条命令一次到达时，按原顺序逐条解析，不能只寻找最后一个端口前缀；
3. 使用合法目标端口前缀和完整命令括号识别边界；
4. 处理括号嵌套、引号和转义，不能简单搜索第一个右括号；
5. 缓存和单条命令均设合理上限；
6. 非法端口、非法命令边界、超长数据和长期无法成帧必须明确报错；
7. 客户端断开后清空其缓存，不能交给新连接；
8. 保持同一客户端的命令顺序；
9. 不因为解析错误自动生成任何 CR3 指令。

CR3 的 29999、30003 回复分别建立独立缓存：

1. 以分号 `;` 识别一条完整回复；
2. 支持回复半包和粘包；
3. 29999 与 30003 不能串口配对；
4. 按各自端口最早的有效待回复请求配对；
5. 超时请求从待回复表删除；
6. 超时后迟到的回复记录为 `orphan_reply`，不得配给新请求；
7. 连接或会话结束时清空缓存和待回复表；
8. 写回上位机时处理短写，不能假设一次 `write()` 完成；
9. 保留旧回复原文，不能伪造成功。

在文档中明确：8888 是兼容通道，没有真正的 request ID；完整请求编号和会话隔离由 8891 v2 提供。

## 5. 可靠解析并重新对齐 CR3 30004

30004 是连续 TCP 字节流，不能假设一次 `readyRead()` 恰好得到 1440 字节。建立持续、有上限的接收缓存并按官方 V3 规则重新对齐。

合法帧条件：

- 固定长度 1440 字节；
- 偏移 0：小端 `uint16`，必须等于 1440；
- 偏移 48：小端 `uint64`，必须等于 `0x0123456789ABCDEF`；
- TestValue 的十进制字符串为 `81985529216486895`。

解析规则：

1. 缓存不足以判断头部时等待下一批数据；
2. 在缓存中寻找同时满足 MessageSize 和 TestValue 的合法帧头；
3. 找不到合法头时，只保留可能与下一批数据组成合法头的末尾 49 字节；
4. 找到合法头但不足 1440 字节时保留并等待；
5. 取出完整 1440 字节后再次校验；
6. 只有校验成功的帧才能更新最新机器人状态；
7. 错误帧、错位字节和非法字段分别计数；
8. 30004 断开时立即清空接收缓存并使最新状态失效；
9. 重连后不得把旧连接残余字节与新连接数据拼接；
10. 缓存达到上限仍找不到合法头时，安全丢弃并记录，不允许无限增长。

字段读取必须使用定宽类型和安全字节转换，例如：

- `quint16`；
- `qint64`；
- `quint64`；
- `quint8`；
- `double`；
- `qFromLittleEndian`；
- 安全的 `memcpy`。

不要使用 Windows `long` 表示协议 64 位字段，不要对未对齐字节直接 `reinterpret_cast`。

字段偏移、类型和单位必须从现场项目中的官方结构、CR3 TCP/IP V3 文档或官方 C++ 示例核实，不得凭印象猜测。单位无法核实时保留原始值并写入待确认清单，不得伪造换算值。

## 6. 建立完整、线程安全的中继机器人状态快照

每个通过校验的 30004 帧生成一次不可变或一致性受保护的最新状态快照。解析线程、UI、UDP 转发和 8891 推送不能读取到一半新一半旧的数据。

每个有效状态递增 `state_seq`。无效帧不能更新最新有效状态，也不能冒充新鲜状态。

完整状态至少包含：

- 原始 `controller_timestamp`；
- 中继接收 `relay_receive_steady_ns`；
- 中继接收 `relay_receive_unix_ns`；
- `message_size`、`test_value`、`robot_mode`；
- `q_actual`、`qd_actual`、`i_actual`、`q_target`；
- `tool_vector_actual`、`tcp_speed_actual`、`tcp_force`、`tool_vector_target`；
- `enable_status`、`drag_status`、`running_status`、`error_status`、`jog_status`；
- `user`、`tool`、`run_queued_cmd`、`pause_cmd_flag`；
- `six_force_online`、`six_force_value`、`actual_quaternion`；
- `load`、`center_xyz`；
- 最新有效状态年龄；
- 30004 连接代次和状态有效标志。

所有数组维度必须准确，所有浮点数必须有限。任何必需字段非法时，该帧不能成为有效状态。

## 7. 新增 TCP 8891 v2 服务

保留 8888，同时新增 TCP 8891。8891 监听器必须在 8888 接受客户端之前或同时准备好，因为 Windows 上位机在 8888 连接成功后只会立即尝试一次 8891，当前不会持续重试。

每个 v2 帧格式固定为：

```text
4 字节大端无符号正文长度 + UTF-8 JSON 正文
```

要求：

- 正文最大 1 MiB；
- 长度为 0、超过上限、非法 JSON、错误版本、错误类型或缺失必填字段时发送 `error` 并关闭异常连接；
- 每个连接维护独立接收缓存；
- 半帧保留，粘连多帧逐条解析；
- 出现无法恢复的协议错误后不能继续猜测边界；
- 发送端处理短写；
- 每个连接使用有界发送队列；
- 慢客户端不能让 `robot_state` 无限积压，状态推送采用“保留最新状态”或其他有界策略；
- 新连接生成新的非空 `session_id`，长度不超过 128；
- 连接关闭后旧 session 立即失效。

公共字段固定为：

```json
{
  "v": 2,
  "type": "消息类型",
  "request_id": "十进制字符串",
  "session_id": "会话编号"
}
```

至少实现并测试：

- `hello`；
- `hello_ack`；
- `command`；
- `reply`；
- `robot_state`；
- `relay_status`；
- `stream_control`；
- `error`。

当前 Windows 上位机实际消费 `hello_ack`、`robot_state`、`relay_status`；不得在本轮强制把上位机现有真机命令切换到 8891。

## 8. 精确实现 8891 握手

Windows 上位机首先发送：

```json
{
  "v": 2,
  "type": "hello",
  "request_id": "1",
  "session_id": "",
  "client": "touchvla_windows",
  "capabilities": ["robot_state", "relay_status"]
}
```

中继必须返回：

```json
{
  "v": 2,
  "type": "hello_ack",
  "request_id": "1",
  "session_id": "中继分配的非空会话编号"
}
```

要求：

- 原样回显 hello 的 `request_id`；
- 每次新连接使用新 session；
- 后续所有推送使用当前 session；
- 主动推送使用 `request_id: "0"`；
- 版本、请求号、空 session 或后续 session 不匹配时拒绝；
- 建议先推送一次 `relay_status`，再推送第一条 `robot_state`；
- 客户端断开、新客户端替换或中继 Stop 时立即使旧 session 失效。

## 9. 严格输出与上位机匹配的 robot_state

每个有效 30004 包生成一条：

```json
{
  "v": 2,
  "type": "robot_state",
  "request_id": "0",
  "session_id": "当前会话",
  "valid": true,
  "source": "cr3_30004",
  "state_seq": "有效状态递增序号",
  "controller_timestamp": "控制器原始时间十进制字符串",
  "relay_receive_steady_ns": "中继单调时钟十进制字符串",
  "relay_receive_unix_ns": "中继 Unix 时间十进制字符串",
  "message_size": 1440,
  "test_value": "81985529216486895",
  "robot_mode": 5,
  "q_actual": [0, 0, 0, 0, 0, 0],
  "qd_actual": [0, 0, 0, 0, 0, 0],
  "i_actual": [0, 0, 0, 0, 0, 0],
  "q_target": [0, 0, 0, 0, 0, 0],
  "tool_vector_actual": [0, 0, 0, 0, 0, 0],
  "tcp_speed_actual": [0, 0, 0, 0, 0, 0],
  "tcp_force": [0, 0, 0, 0, 0, 0],
  "tool_vector_target": [0, 0, 0, 0, 0, 0],
  "six_force_value": [0, 0, 0, 0, 0, 0],
  "actual_quaternion": [1, 0, 0, 0],
  "center_xyz": [0, 0, 0],
  "load": 0.0,
  "user": 0,
  "tool": 0,
  "enable_status": false,
  "drag_status": false,
  "running_status": false,
  "error_status": false,
  "jog_status": false,
  "run_queued_cmd": false,
  "pause_cmd_flag": false,
  "six_force_online": false
}
```

示例数值只表示 JSON 类型，实际值必须来自同一个已验证的 30004 帧。

字段名必须完全一致。规则：

- 64 位序号和时间使用十进制字符串；
- `message_size` 是 JSON 整数 1440；
- `test_value` 是固定十进制字符串；
- 状态开关使用 JSON 布尔值，不能使用字符串 `"0"`/`"1"`；
- 所有数组长度必须准确；
- 所有数值必须有限；
- 状态必须来自同一个 30004 快照，不能从 UI 文本反向拼装；
- 如果已可靠确认控制器时间单位并换算成纳秒，可额外发送字符串 `source_timestamp_ns`；未确认时不要发送该字段。

## 10. 实现 relay_status 和真实 relay_ready

Windows 上位机固定读取：

```json
{
  "v": 2,
  "type": "relay_status",
  "request_id": "0",
  "session_id": "当前会话",
  "port_29999_connected": true,
  "port_30003_connected": true,
  "port_30004_connected": true,
  "relay_ready": true,
  "last_valid_state_age_ms": 1.0,
  "state_seq": "123"
}
```

字段名不能修改。`relay_ready=true` 只允许在以下条件同时成立时出现：

1. 29999 已连接；
2. 30003 已连接；
3. 30004 已连接；
4. 最近 500 ms 内收到通过 MessageSize/TestValue 和字段校验的有效 30004 状态。

任一条件失效时：

- 立即把 ready 变为 false；
- 立即推送新的 `relay_status`；
- 清楚记录失效原因；
- 不能等下一次 UI 刷新；
- 旧状态不能继续冒充 Ready。

周期性推送 `relay_status` 心跳。Windows 超过 500 ms 会判定它过期，因此建议使用 200 ms 周期。若从未收到有效状态，使用 `state_seq: "0"`、`relay_ready: false`，状态年龄使用有限、非负的大数，不能使用 null、NaN 或 Infinity。

可以附加：

- 30004 错位/非法/重新对齐计数；
- 29999/30003 orphan reply 计数；
- 8888/8891 客户端状态；
- Twin、Force、Video 是否开启；
- 相机帧序号和年龄；
- 当前会话编号。

但这些附加项不能代替固定字段和 Ready 条件。

## 11. 可靠 v2 command/reply

在不影响 8888 的前提下，为未来 Windows 命令迁移实现 8891 `command/reply`。

命令示例：

```json
{
  "v": 2,
  "type": "command",
  "request_id": "42",
  "session_id": "当前会话",
  "target_port": 29999,
  "command": "RobotMode()",
  "client_send_unix_ns": "可选十进制字符串"
}
```

回复示例：

```json
{
  "v": 2,
  "type": "reply",
  "request_id": "42",
  "session_id": "当前会话",
  "target_port": 29999,
  "command_name": "RobotMode",
  "controller_code": 0,
  "raw_reply": "0,{5},RobotMode();",
  "relay_receive_steady_ns": "十进制字符串",
  "robot_reply_receive_steady_ns": "十进制字符串"
}
```

要求：

- 当前只允许目标端口 29999、30003；
- 命令非空且长度受限；
- 一条 command 只产生一条最终 reply 或 error；
- 29999、30003 分别维护 FIFO 待回复请求；
- CR3 回复按分号完整成帧后再配对；
- 超时请求删除，迟到回复成为 orphan；
- 旧 session 的 reply 不得进入新 session；
- reply 保留控制器真实原文和返回码；
- 不伪造“机器人到达”，Socket 写入、控制器接收和机器人实际状态必须使用不同语义；
- 当前 Windows 尚未把真机命令切换到 8891，因此只通过模拟 Socket 自动测试，不改变默认生产路径。

## 12. 为 ACT 连续动作准备中继侧软件语义

不启动真机 Auto，但完成中继侧可离线实现的防旧动作基础：

1. v2 运动请求可以携带可选 `control_generation`、`action_seq`、`ttl_ms`；
2. 这些 64 位编号使用十进制字符串；
3. 同一 session、同一 generation 内拒绝倒退或重复的 action_seq；
4. ttl 从中继收到该请求时开始计算，不比较不同电脑的 steady_clock 数值；
5. 已过期的未发送动作不得写入 CR3 Socket；
6. 新 session、Stop、断线和 generation 变化立即清除未发送动作；
7. 对可替换的连续目标使用“只保留最新未发送目标”，不能无限 FIFO 积压；
8. 已经写入 CR3 Socket 的动作不能伪装成已撤回，状态文档必须说明这一边界；
9. 不允许 Linux 或其他客户端绕过 Windows 会话直接建立生产运动通道；
10. 默认保持该 v2 运动路径非生产启用状态，等 Windows 端完成迁移和现场授权后再启用。

为 `MovL` 和未来 `ServoP` 区分回复方式：

- MovL：可以等待控制器普通回复，但该回复只表示命令被接收；
- ServoP：协议没有普通命令回复，不能复用“发一条、等普通回复、再发下一条”的循环；
- 可以在软件层准备 `write_ack` 或等价状态，明确表示“字节已写入机器人 Socket”，但不得称为控制器接收或机器人到达；
- ServoP 真实周期、停止方式和运动效果必须留待低速真机专项验证；
- 本轮不得把现有 Manual 或 Auto 切换到 ServoP。

## 13. 保留并加固 Twin 和 Force 兼容输出

旧 Twin 格式必须保持：

```text
TWIN:x,y,z,rx,ry,rz|j1,j2,j3,j4,j5,j6
```

要求：

- 使用 UDP 源端口 8889；
- 位姿和关节来自同一个已校验 30004 快照；
- 全部值为有限数；
- 不从 UI 文本反向拼装；
- `TWIN_STOP` 后停止发送旧包；
- 新订阅不能收到旧会话残留数据。

旧 Force 格式必须保持：

```text
FORCE:fx,fy,fz,mx,my,mz|Q:qw,qx,qy,qz
```

要求：

- 使用 UDP 源端口 8887；
- 六维力和四元数来自同一个已校验 30004 快照；
- 必须带有效 `Q:` 四元数，否则不发送可用 Force 包；
- 全部值为有限数；
- `FORCE_STOP` 后停止发送旧包；
- 新订阅不能复用停止前排队的数据；
- 不擅自改变现有坐标轴映射。若发现映射可能错误，只记录证据和待现场验证项，另行处理。

## 14. 视频和 ACT 图像链的软件准备

保留现有 UDP 8886 视频功能，不在没有上位机同步修改时破坏旧视频协议。完成中继侧可离线准备：

1. 为相机帧生成递增 `frame_seq`；
2. 保存中继采集时间、编码完成时间和发送时间；
3. 保存宽、高、编码格式和字节数；
4. 记录捕获失败、编码失败、发送失败和丢弃计数；
5. 发送队列使用“最新帧优先”或有界队列，不能让旧视频无限积压；
6. Stop、客户端断开和新会话后清空旧视频队列；
7. 把相机状态和最新帧年龄作为 `relay_status` 的可选诊断字段；
8. 为未来上位机数据采集写清楚元数据字段和单位；
9. 不把“UDP write 成功”写成“上位机已收到完整图像”；
10. 不在没有实测丢帧数据时强制切换视频传输方式。

如果单帧 JPG 可能接近 UDP 64 KiB，只实现并测试一个与旧 8886 并行、默认关闭的分片/重组协议或带长度头 TCP 原型，并完整记录协议；不得直接替换旧生产视频通道。真实丢帧率和最终传输方式留到现场长时间测试后决定。

## 15. 夹爪软件占位

夹爪尚未到货，不得猜测型号、通信接口、寄存器、开合命令、范围或反馈能力。中继侧只做非运行占位：

- 配置中明确 `installed=false`、`configured=false`；
- 状态中明确夹爪不可用，不发送伪造位置；
- 如现有架构适合，可建立禁用状态的 GripperAdapter 接口；
- 未配置时任何夹爪命令必须被拒绝；
- 不发数字/模拟输出试探硬件；
- 将型号、接口、命令、范围、负载、重心和 Tool/TCP 列为到货后待填项目。

## 16. 网络、状态和诊断指标

增加线程安全、不会阻塞 30004 高频接收的诊断计数：

- 每个端口连接状态和最近变化时间；
- 30004 接收字节、有效帧、非法帧、错位字节、重同步次数；
- 最新有效状态序号和年龄；
- 8888 半包、粘包、超长和解析失败；
- 29999/30003 回复超时和 orphan；
- 8891 当前 session、握手、协议拒绝、接收/发送帧；
- 8891 慢客户端状态丢弃/覆盖次数；
- Twin、Force、Video 订阅状态和发送计数；
- 相机最新帧序号、年龄、大小和错误计数；
- Stop、客户端断开和会话切换次数。

日志必须限频，不能在 125 Hz 路径逐帧刷 UI 或磁盘。高频路径只更新轻量计数或内存快照，低频定时器再刷新 UI。

## 17. Stop、断线和资源释放

Stop、8888 客户端断开、8891 客户端断开、中继关闭或客户端替换时建立明确流程：

1. 使旧会话失效；
2. 停止接受旧会话新命令；
3. 清除未发送动作和待回复请求；
4. 清除各连接接收缓存和发送队列；
5. 停止向旧客户端推送 robot_state、relay_status、Twin、Force 和 Video；
6. 停止相关心跳和定时器；
7. 最后释放属于该客户端的 Socket 和线程资源；
8. 新连接不能继承旧请求、旧回复、旧状态或旧 UDP 数据；
9. 中继不得在断开时自行 ClearError、EnableRobot 或重放 MovL；
10. 网络 Stop 不能在 UI 或日志中冒充实体急停。

如果 Windows 明确发送 `DisableRobot()`：

- 可靠转发给 29999；
- 等待真实控制器回复；
- 原样返回结果；
- 不在中继本地提前伪造成功。

## 18. 必须完成的离线自动测试

不得连接真实 CR3。使用模拟 Socket、内存输入、假 30004 数据和本机回环完成测试，至少覆盖：

### 8888 和 CR3 回复

1. 一条旧命令分成三次输入，只解析出一条；
2. 三条旧命令一次输入，按顺序解析三条；
3. 括号、引号和转义不会错误截断；
4. 非法端口和超长命令被拒绝；
5. 29999 回复半包正确重组；
6. 30003 多条回复粘包正确拆分；
7. 两个端口回复不会串配；
8. 超时迟到回复成为 orphan，不会配给新请求；
9. 短写能够继续发送剩余字节。

### 30004

10. 一帧分成多次输入后正确重组；
11. 两帧粘连时解析两帧；
12. 帧前插入 1～1439 个垃圾字节后重新对齐；
13. 错误 MessageSize 不更新状态；
14. 错误 TestValue 不更新状态；
15. 非有限必需字段不能产生有效状态；
16. 断开重连后不复用旧缓存；
17. 多线程读取状态时不会得到混合快照。

### 8891

18. 长度帧分成多次输入后只解析一帧；
19. 多帧粘连按顺序解析；
20. 长度 0、超过 1 MiB、非法 JSON、缺失字段被拒绝；
21. hello_ack 正确回显 request_id；
22. 每次连接分配不同 session；
23. 错 session 和旧 session 消息被拒绝；
24. 主动推送使用 request_id `"0"`；
25. 慢客户端不会无限积压 robot_state；
26. 8891 在 8888 接受客户端前或同时监听；
27. command/reply 保留 request_id 和真实原始回复；
28. 超时请求与迟到回复隔离。

### Ready、动作和停止

29. 29999、30003、30004 任一断开时 ready=false；
30. 状态超过 500 ms 时 ready=false；
31. 200 ms 状态心跳不会被上位机 500 ms 门槛判旧；
32. 新 session 清除旧动作和旧请求；
33. action_seq 倒退或重复被拒绝；
34. ttl 到期的未发送动作不会写入假 CR3 Socket；
35. 最新目标覆盖旧的未发送连续目标；
36. ServoP 测试路径不会等待普通 CR3 回复；
37. Stop 后没有旧状态、旧回复或旧 UDP 包进入新会话。

### Twin、Force、Video 和占位

38. Twin 格式与旧上位机兼容；
39. Force 包包含来自同一状态的有效四元数；
40. FORCE_STOP/TWIN_STOP 后不继续发送旧包；
41. 视频队列有界并优先保留新帧；
42. 视频元数据序号和时间递增合理；
43. 未配置夹爪命令被拒绝；
44. 旧 8888、Twin、Force、Video 和现有 UI 启停时不崩溃。

如果现有工程没有测试框架，可以新增最小离线测试目标或独立测试程序；测试必须可重复执行并有非零失败退出码。

## 19. 构建和质量检查

完成后：

- 按现场工程支持的配置构建 Debug 和 Release；
- 如只能构建某一种配置，记录客观原因；
- 运行全部新增离线测试；
- 检查警告、线程退出、Socket 析构和定时器生命周期；
- 检查高频 30004 路径没有逐帧 UI、磁盘或阻塞日志；
- 检查所有发送队列和接收缓存都有上限；
- 检查没有自动 ClearError/EnableRobot；
- 检查没有默认启用 ServoP 或新视频协议；
- 检查没有提交构建产物和用户无关文件。

构建通过只能写成构建验证，模拟测试通过只能写成离线软件验证，不能冒充真机验证。

## 20. 分步自动提交

请按可独立回退的小步骤提交，建议顺序：

1. `test(relay): add offline protocol and 30004 fixtures`
2. `fix(relay): frame legacy commands and robot replies`
3. `fix(relay): validate and resync CR3 feedback stream`
4. `feat(relay): publish complete robot state snapshot`
5. `feat(relay): add v2 8891 framed session transport`
6. `feat(relay): add relay ready and stale-state handling`
7. `feat(relay): add safe v2 command and latest-target semantics`
8. `fix(relay): isolate stop and reconnect sessions`
9. `feat(relay): add bounded stream diagnostics and camera metadata`
10. `test(relay): cover compatibility and failure paths`
11. `docs(relay): record ACT relay status and onsite checklist`

可以根据现场文件结构调整拆分，但不得把所有功能压成一个提交。每次提交前确认：

- 当前分支是 `vla_yj_relay`；
- 只暂存本步骤文件；
- 对应测试通过；
- 没有构建产物和无关文件。

纯软件工作完成后自动提交，不要只留下未提交修改。

## 21. 创建并维护中继状态文档

在中继仓库创建或更新：

```text
docs/TouchVLA_Relay_Status.md
```

状态文档必须面向不了解代码的人，用通俗中文编写，并至少包含：

1. 当前中继总体执行流；
2. 当前分支和基线；
3. 已完成的软件功能；
4. 自动测试和构建结果；
5. 尚未完成或需要现场验证的事项；
6. 当前安全限制；
7. 8888、8891、29999、30003、30004、8886、8887、8889、8890 的用途；
8. 旧协议与 v2 协议的兼容关系；
9. Windows 上位机接入方式；
10. 每个提交哈希和修改文件。

每一项功能修改都必须使用以下固定结构：

```text
### 修改项名称

修改前：
- 原流程怎样运行；
- 原问题是什么。

修改后：
- 新流程怎样运行；
- 哪些函数、模块和协议发生变化。

好处：
- 具体解决了什么问题；
- 在什么运行、断线、Stop、重连或异常场景生效。

验证：
- 自动测试命令和结果；
- 构建命令和结果；
- 明确哪些尚未真机验证。

风险和回退：
- 剩余限制；
- 对应独立提交和回退方式。
```

不得使用“应该没问题”“已经完全安全”等没有证据的结论。

## 22. 必须留到现场的事项

以下项目只准备日志、诊断和测试步骤，不得伪造完成：

- 当前运行 exe 是否确实来自 `vla_yj_relay`；
- 真实 CR3 30004 字段偏移、单位和实际约 125 Hz 行为；
- 三个机器人端口真实断线和重连；
- Windows 上位机与真实 8891 的握手和持续状态；
- 真实 8888 命令半包、粘包和迟到同名回复；
- Touch 松手后是否仍有旧 MovL；
- Stop 时 DisableRobot 回复和关闭顺序；
- 报警后是否绝不自动 ClearError/EnableRobot；
- Force 四元数、坐标轴和实际力方向；
- 相机实际帧率、延迟、丢帧和 30 分钟稳定性；
- ServoP 周期、停止、报警和真机运动；
- 夹爪型号、接口、命令、范围和反馈；
- User、Tool、TCP、负载、重心、速度和工作空间；
- 实际停止距离、制动余量、急停和现场障碍物。

为这些事项提供清晰测试步骤和成功现象，但不要自动连接、使能或运动 CR3。

## 23. 最终交付

所有可离线完成的软件工作、测试、构建和提交结束后，向用户提供：

1. 现场源码绝对路径；
2. 当前分支，必须为 `vla_yj_relay`；
3. 基线提交；
4. 所有新提交的哈希和说明；
5. 修改文件列表；
6. 自动测试命令、测试数量和结果；
7. Debug/Release 构建命令和结果；
8. `docs/TouchVLA_Relay_Status.md` 路径；
9. 8888 兼容协议说明；
10. 8891 v2 协议和示例；
11. 30004 重同步说明；
12. `robot_state` 和 `relay_status` 实际 JSON 样例；
13. Twin、Force 和视频元数据样例；
14. 尚未真机验证清单；
15. 与任务描述不一致的现场源码事实；
16. 工作区最终 `git status`，并说明任何保留的用户修改。

最后必须明确提醒用户：

```text
请把以下内容带回 Windows 上位机项目的 Codex 进行两边协议复核：
1. vla_yj_relay 分支名；
2. 全部提交哈希；
3. docs/TouchVLA_Relay_Status.md；
4. robot_state 和 relay_status 样例；
5. 自动测试与构建结果；
6. 尚未真机验证清单。
```

完成纯软件任务后停止，不擅自启动、使能或运动真实 CR3，不把未进行的真机验证写成通过。
