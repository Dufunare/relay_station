# TouchVLA 中继程序状态与完整变更记录

更新日期：2026-09-08（Asia/Shanghai）

源码目录：`D:\ACN\Relay_Station_QT`

当前开发分支：`vla_yj_relay`

对比基准：`ORAN_version_UDP_V2@661de3c57049a46655daf68eb04e30d04ab701d5`

本文重写前 HEAD：`1851e374ddfe1e9e046474cbb550a44b60fd519b`

本次 EnableRobot 期限代码提交：`c695d98446e9f37ec6d198c2ae446dddc28e5916`

> 本文提交不能在自身内容中预先写出自己的 Git 哈希。重写后的最终提交号以 `git log -1 --oneline` 和交付报告为准。

## 1. 核心结论

- 当前程序的正式工程名、窗口类和输出名已经统一为 `relay_station` / `RelayStation`；
- `vla_yj_relay` 与 `ORAN_version_UDP_V2` 是从共同祖先分叉的两个分支，不是前者线性继承后者；
- 当前分支在旧 8888 TCP 兼容链路之外，新增了可靠的 8891 v2 会话/状态协议、严格的 30004 解析、请求回复隔离、统一状态快照、有界视频队列、完整诊断和离线测试；
- 8891 v2 运动、ServoP、视频分片生产通道和未配置夹爪仍保持禁用；旧 8888 TCP 仍可转发除 ServoP 外的合法 CR3 命令，因此运行程序不等于允许无人值守运动；
- 软件 Stop 只清理网络、队列、状态和订阅，不是机器人实体急停；
- 离线测试为 `64/64` 通过，Debug/Release 均已在当前源码路径完成本次完整重建；尚未完成 CR3、Touch、相机、网络抖动和机械安全真机验收；
- 除现场起点 `8420d66` 同时是 `ORAN_version_UDP` 分支头外，后续 TouchVLA 实现提交和本次文档提交都只落在 `vla_yj_relay`；没有改写或移动其他分支指针。

## 2. 版本关系与比较方法

### 2.1 分支关系

```text
ecb4afa  共同祖先（2026-04-09）
├─ 661de3c  ORAN_version_UDP_V2
│             └─ 增加 UDP 8888 主控入口，与 TCP 8888 并存
└─ 8420d66  ORAN_version_UDP
              └─ 增加 Force UDP 输出，成为 vla_yj_relay 的现场起点
                 └─ 1ee0a90 ... 1851e37  TouchVLA 中继改造
```

因此，“相比 `ORAN_version_UDP_V2` 的修改”采用以下口径：

1. 最终净差异用 `git diff ORAN_version_UDP_V2 vla_yj_relay`；
2. 时间顺序用 `git log --reverse ORAN_version_UDP_V2..vla_yj_relay`；
3. 第一项先说明两个兄弟分支对 8888/Force 的不同选择，后续再按当前分支提交时间记录；
4. “修改前”指 V2 或紧邻上一阶段的状态，“修改后”指该阶段完成后的状态；
5. 文档和许可文件不冒充业务代码，但仍在工程化时间线中记录。

### 2.2 基准和当前版本

| 项目 | 值 |
|---|---|
| V2 基准 | `661de3c57049a46655daf68eb04e30d04ab701d5` |
| 共同祖先 | `ecb4afa0f468156336ce42d84f961d4262fe8948` |
| 当前分支 | `vla_yj_relay` |
| 本文重写前 HEAD | `1851e374ddfe1e9e046474cbb550a44b60fd519b` |
| 本次功能代码提交 | `c695d98446e9f37ec6d198c2ae446dddc28e5916` |
| V2 独有提交 | `661de3c`，不属于当前 HEAD 的祖先 |
| 当前现场起点 | `8420d66`，也是 `ORAN_version_UDP` 分支头 |
| 当前工作区既有未跟踪文件 | `Relay_Station_B3_Modification_Prompt.md`，不纳入提交 |

## 3. 当前核心执行流

### 3.1 启动和并行链路

```text
main.cpp
  → RelayStation 构造 UI、UDP 兼容流和 RelayRuntime
  → 操作者点击 Start
  → RelayStation::on_btn_start_clicked()
  → RelayRuntime::start()
      ① 先监听 TCP 8891
      ② 再监听 TCP 8888
      ③ 异步连接 CR3 29999 / 30003 / 30004
      ④ 启动 100 ms 请求过期、200 ms 状态心跳、501 ms 状态失效检查

并行链路 A：Windows TCP 8888
  → LegacyCommandFramer
  → routeLegacyCommand
  → CR3 29999 或 30003
  → RobotReplyFramer + PendingReplyLedger
  → 原始 CR3 回复返回同一代 Windows 连接

并行链路 B：Windows TCP 8891
  → 4 字节大端长度 + JSON
  → hello / session / request_id
  → relay_status、robot_state、非运动 command 或 stream_control
  → v2 运动在写 CR3 前被生产总闸拒绝

并行链路 C：CR3 TCP 30004
  → FeedbackStreamParser 校验并重同步 1440 字节帧
  → RobotStateStore 发布整份 RobotStateSnapshot
  ├─ 推送 8891 robot_state
  ├─ 生成 Twin UDP
  ├─ 生成 Force UDP
  └─ 更新 UI 和 relay_ready

并行链路 D：相机 UDP 8886
  → VIDEO_START 记录订阅方
  → VideoStreamThread 采集并编码 JPG
  → LatestFrameQueue 只保留最新一帧
  → 主线程发送旧格式单包 UDP JPG
```

### 3.2 端口

| 端口 | 方向/协议 | 当前用途 |
|---:|---|---|
| 29999 | 中继 ↔ CR3，TCP | Dashboard/使能类普通指令和真实回复 |
| 30003 | 中继 ↔ CR3，TCP | 运动类普通指令和真实回复 |
| 30004 | CR3 → 中继，TCP 流 | 1440 字节 V3 实时状态 |
| 8888 | Windows ↔ 中继，TCP | 旧兼容命令入口；当前不再提供 V2 分支的 UDP 8888 控制入口 |
| 8891 | Windows ↔ 中继，TCP | v2 长度帧 JSON、会话、状态、诊断和受限命令 |
| 8889 | Windows ↔ 中继，UDP | Twin 订阅与状态输出，实际值由 UI 配置端口决定 |
| 8887 | Windows ↔ 中继，UDP | Force 订阅与六维力/四元数输出 |
| 8886 | Windows ↔ 中继，UDP | 视频控制与旧格式单包 JPG 输出 |
| 8892 | 原型，UDP | TVF2 视频分片实验端口；生产禁用 |

## 4. 相比 ORAN_version_UDP_V2 的按时间修改记录

以下条目按当前分支提交时间排列。每一项都明确给出修改前、修改后、好处和可核对位置。

### 4.1 2026-05-26 16:56 — 分支路线改变：TCP 8888 + Force

提交：`8420d66eb4a1211f97a38ec0d648a26ebe8457d1`（`完成力反馈到touch`）

特殊说明：该提交不是 `661de3c` 的子提交；两者是共同祖先之后的兄弟路线。本节记录二者最终代码差异。

**修改前（ORAN_version_UDP_V2）**

- TCP 8888 与 UDP 8888 同时作为主控入口；
- UDP 8888 由 `m_udpControlSocket`、`onUdpControlReadyRead()` 接收 `30003|MovL(...)` 并直接写运动端口；
- 没有 Force UDP 订阅和输出；
- 30004 仍只是每 1440 字节切包并直接读取结构字段。

**修改后（当前路线起点）**

- 删除 UDP 8888 控制 Socket 和 `onUdpControlReadyRead()`，保留 TCP 8888；
- 新增 UDP 8887 Force Socket、`FORCE_START/FORCE_STOP`、2 秒订阅超时；
- 从 30004 数据中输出六维力和四元数；
- 对 29999 写入和向 Windows 返回 29999/30003 回复增加 `flush()`。

**好处**

- 消除了同一逻辑端口上第二条未经分帧和回复关联的 UDP 运动入口；
- Touch 获得 Force 输出通道；
- 这是功能起点，不代表当时已经解决 30004 校验、混帧、请求隔离或真机安全问题，后续提交继续修复。

**关键历史文件/函数**

- `test1.cpp/.h`；
- `onUdpForceReadyRead()`、`onRobotStatusReadyRead()`；
- 删除的 `onUdpControlReadyRead()`。

### 4.2 2026-09-03 21:13 — 建立可测试的协议核心

提交：`1ee0a908064721ed202179b564d3545396d77f69`

**修改前**

- 网络解析、状态读取、视频和 UI 高度耦合在 `test1.cpp/.h`；
- TCP 8888 把一次 `readAll()` 当成一条完整命令，不能可靠处理半包和粘包；
- CR3 回复没有独立分帧、请求表、超时债务和跨端口隔离；
- 30004 只按固定大小切块，不校验 `MessageSize`、`TestValue`、有限浮点或布尔字段；
- 没有可脱离真实机器人运行的协议测试目标。

**修改后**

- 新增 `relay_core.cpp/.h`，把协议和状态算法从 UI 中抽出；
- `LegacyCommandFramer` 支持半包、粘包、嵌套括号、引号和转义；命令上限 64 KiB、总缓存上限 256 KiB、未完成命令 2 秒过期；
- `RobotReplyFramer` 按 CR3 分号边界切回复；
- `PendingReplyLedger` 对 29999、30003 分别维护 FIFO、3 秒超时和 orphan debt；
- `ShortWriteBuffer` 处理 Socket 短写并限制累计输出；
- `FeedbackStreamParser` 搜索合法 1440 字节帧头、重同步、验证字段并分类计数；
- `RobotStateStore` 通过读写锁整份发布 `RobotStateSnapshot`；
- 加入 `V2FrameParser`、`LatestActionGate`、`LatestFrameQueue`、`GripperAdapter` 等纯算法基础；
- 新增合成 30004 帧和协议核心离线测试。

**好处**

- TCP 边界、回复顺序、状态一致性和内存上限变成可明确验证的规则；
- 关键失败路径不必连接 CR3 即可重复测试；
- 为后续运行时接入提供单一协议实现，减少 UI 回调里各写一套解析逻辑。

**关键文件/函数**

- `relay_core.cpp/.h`；
- `LegacyCommandFramer::feed()/expireIncomplete()`；
- `RobotReplyFramer::feed()`；
- `PendingReplyLedger::enqueue()/match()/expire()`；
- `FeedbackStreamParser::feed()/findHeader()/parseFrame()`；
- `RobotStateStore::publish()/latest()/invalidate()`；
- `V2FrameParser::feed()`、`LatestActionGate::offer()`、`LatestFrameQueue::offer()`；
- `tests/relay_core_tests.cpp/.pro`。

### 4.3 2026-09-03 21:34 — 加入 RelayRuntime 和 8891 v2 服务

提交：`5142d19454a219d2794277699e71945c3b2ea316`

**修改前**

- 上一阶段已有独立算法，但生产 UI 仍由旧 Socket 回调直接收发；
- 没有运行中的 TCP 8891 服务、UUID 会话、状态推送或统一生命周期；
- 单个 CR3 端口连接或旧状态可能被误当成可用状态。

**修改后**

- 新增 `relay_runtime.cpp/.h`，统一拥有 TCP 8888、TCP 8891 和 CR3 三个 TCP Socket；
- `start()` 先监听 8891，再监听 8888，然后异步连接 29999/30003/30004；断线后每秒只重连未连接端口；
- 8891 使用“4 字节大端正文长度 + UTF-8 JSON”，正文最大 1 MiB；
- 第一条消息必须是精确 `hello`，每个连接生成新 UUID session，`hello_ack` 回显 request ID；
- 8891 状态和普通输出队列有界；慢客户端的 state/status 只保留最新值；
- 8888 请求和 v2 请求共用分端口回复表，但通过 connection generation/session 隔离；
- 30004 合法快照用于 8891 `robot_state` 和 `relay_status`；
- `relay_ready=true` 必须同时满足六个布尔条件：29999、30003、30004 分别已连接，快照有效，连接代次非零，状态年龄不超过 500 ms；
- 200 ms 推送状态心跳，合法状态后 501 ms 定时触发过期检查；
- v2 动作引入 `control_generation`、递增 `action_seq` 和 TTL，但 `kV2MotionProductionEnabled=false`，生产运动在写 CR3 前拒绝；
- 加入 TVF2 视频分片和乱序重组算法，但 `kVideoFragmentationProductionEnabled=false`，没有接入生产 UDP 发送；
- Stop、连接替换和断线清理 parser、回复表、短写队列、状态、动作和会话。

**好处**

- Windows 可以用 session/request ID 判断回复归属，不再让旧连接数据进入新连接；
- “网线连接”与“状态足够新且可用于闭环”分离；
- 运动协议可以先离线验证，同时保持生产总闸关闭；
- 三个 CR3 端口的生命周期、超时和错误返回集中管理。

**关键文件/函数**

- `relay_runtime.cpp/.h`；
- `RelayRuntime::start()/stop()`；
- `acceptLegacyClient()/acceptV2Client()`；
- `readLegacyClient()/readV2Client()`；
- `handleHello()/handleV2Command()/handleStreamControl()`；
- `routeToRobot()/handleRobotReplies()/deliverReply()`；
- `readFeedback()/publishRobotState()/publishRelayStatus()`；
- `expireRequests()/clearSessionData()`。
- `relay_core.cpp/.h`：`VideoFragmentReassembler::feed()/clear()` 和 TVF2 帧约束。

### 4.4 2026-09-03 21:38 — 把新运行时接入 UI，隔离 Stop/重连和兼容流

提交：`8996555b9f98293141440dbaf4901d2ea21dce60`

**修改前**

- `RelayRuntime` 已存在，但 `test1.cpp/.h` 仍保留大量旧的服务器、CR3 Socket、状态缓冲和直接转发逻辑；
- 新旧路径同时存在会造成重复所有权、重复读取或清理遗漏；
- Twin/Force/Video 订阅目标和线程队列没有统一跟随 session/Stop 清理；
- 夹爪、v2 运动和视频分片的生产开关没有集中配置文件。

**修改后**

- UI 创建单一 `RelayRuntime`，通过信号接收日志、状态、连接、命令和回复事件；
- Start 调用 `RelayRuntime::start()`，Stop 调用 `RelayRuntime::stop()`；删除 UI 中重复的 CR3 TCP 和 8888 TCP 所有权；
- `sessionReset` 触发 `stopCompatibilityStreams()`，清除 Twin、Force、Video 目标、定时器和待发帧；
- Twin/Force 改从 `RobotStateStore` 的同一合法快照生成，不再从 UI 的局部字节缓冲读取；
- `VideoStreamThread` 不再把每个编码帧直接排入 Qt 事件队列，而是写入 `LatestFrameQueue`；主线程每次只取最新一帧；
- 视频订阅替换时停止旧线程、清旧队列，再开始新订阅；
- 新增 `relay_config.json` 作为部署声明，写明 gripper、v2 motion、TVF2 production 均为 false；运行时没有动态解析该 JSON，真正的禁用由编译常量和默认 `GripperAdapter` 配置执行；
- 窗口析构函数在运行时或测速线程仍活动时调用统一 Stop，避免直接关窗口遗漏清理；
- 主工程加入 `relay_core`、`relay_runtime` 和相应 Qt MOC 项。

**好处**

- 一个网络资源只有一个明确所有者，避免旧 UI 路径和新运行时争用；
- 新 session、重连和 Stop 不继承旧请求、状态、UDP 地址或视频帧；
- Twin 与 Force 来自同一时刻的完整状态快照，避免新旧字段混合；
- 未验收功能的禁用状态可被源码和配置同时核对。

**关键文件/函数**

- 当时的 `test1.cpp/.h/.vcxproj/.filters`，现为 `relay_station.*`；
- `RelayStation::on_btn_start_clicked()/on_btn_stop_clicked()`；
- `onRobotStateAvailable()`、`ensureVideoThread()`、`stopCompatibilityStreams()`；
- `RelayRuntime::clearSessionData()`；
- `relay_config.json`。

### 4.5 2026-09-03 21:45 — 有界视频、相机元数据和流诊断

提交：`1f03d422241bb86cf43a9c5b58d0bfeaffc5270b`

**修改前**

- 上一提交已接入容量为一的最新帧队列，但订阅方变化时没有先停止旧流；
- 相机诊断只包含少量计数，未完整上报编码/发送时间、尺寸、编码类型和主线程丢弃；
- 活动中的 v2 request ID 还可能跨 29999/30003 重复，客户端发送时间字段缺少严格格式检查；
- `relay_status` 对 8888/8891、端口变化、回复超时和流发送数诊断不足。

**修改后**

- 保留容量固定为一帧的 `LatestFrameQueue`，订阅方变化时先停止旧流再切换地址；
- 完整上报 `frame_seq`、capture/encoded/send steady ns、宽高、`jpeg`、字节数和 dispatch drop；
- `StreamDiagnostics` 补齐订阅状态、发送数、最后帧信息、采集/编码/发送失败和总 drop；
- `PendingReplyLedger::hasV2Request()` 拒绝同一 session 中仍在等待回复的重复 request ID；
- 可选 `client_send_unix_ns` 必须是无符号十进制字符串，否则按协议错误关闭 v2 连接；
- `syncStreamDiagnostics()` 把 UI 兼容流统计同步给 `RelayRuntime::enrichedRelayStatus()`；
- 保留 UDP 8886 旧格式单包 JPG；大于约 65500 字节的帧拒绝发送并计数；
- TVF2 分片/乱序重组继续只作为离线原型，生产端口 8892 保持关闭；
- 8891、回复超时、端口变化和流发送诊断进一步补全。

**好处**

- 视频延迟不会因为无界排队持续增加，接收端看到的是尽可能新的画面；
- 现场可以区分“相机没采到”“编码失败”“本机发送失败”和“旧帧被新帧替换”；
- 同一活动 request ID 不会在另一个 CR3 端口再次占用，客户端时间戳也不会以错误类型混入协议；
- 在不破坏旧 Windows UDP 8886 格式的前提下准备后续分片协议。

**关键文件/函数**

- `relay_core.cpp/.h`：`PendingReplyLedger::hasV2Request()`；
- `relay_station.cpp/.h`：`onUdpCommandReadyRead()`、`ensureVideoThread()`、`syncStreamDiagnostics()`；
- `relay_runtime.cpp/.h`：`handleV2Command()`、`setStreamDiagnostics()`、`enrichedRelayStatus()`。

### 4.6 2026-09-03 21:46 — 补齐失败路径和一键离线测试

提交：`2a19e279e613b2e648ceda8daf7a2344892da8d4`

**修改前**

- 已有核心测试，但缺少统一定位 Qt/MSBuild、构建并运行测试的 Windows 入口；
- 部分兼容和失败生命周期还没有回归用例。

**修改后**

- 新增 `tests/run_offline_tests.ps1`；
- 自动定位开发环境、用 qmake 生成测试工程、构建并运行；构建或任一测试失败都返回非零；
- 补充兼容 parser 生命周期和运行时失败场景；最终测试集扩展为 59 项。

**好处**

- 不连接机器人即可重复执行同一套回归检查；
- 协议修改不再只依赖人工阅读和现场试错。

**关键文件/入口**

- `tests/relay_core_tests.cpp`；
- `tests/relay_core_tests.pro`；
- `tests/run_offline_tests.ps1`。

### 4.7 2026-09-03 22:06 — 补齐生命周期和错误分类诊断

提交：`d187ef252b0d7baa5abb9ec89425e453dbcd5522`

**修改前**

- `relay_status` 已有基本连接、帧、超时和流统计，但不能完整区分 30004 失败类型、客户端生命周期和发送拒绝。

**修改后**

- 增加 30004 `message_size`、`test_value`、非有限浮点、非法布尔、buffer drop 分类；
- 增加 8888/8891 当前连接、连接次数、断开次数、握手状态；
- 增加 v2 send reject、robot write reject、Stop 次数、session 切换、动作过期和动作替换；
- 保留三个 CR3 端口最近变化的 steady 时间。

**好处**

- 同一个“Not Ready”可以继续定位为端口、帧格式、状态过期、队列、会话或发送层问题；
- 高频 30004 错误仍以计数为主，UI 日志每秒最多提示一次，避免日志本身拖慢程序。

**关键文件/函数**

- `relay_runtime.cpp`；
- `RelayRuntime::enrichedRelayStatus()`；
- `markPortChanged()`、`emitFeedbackErrorLimited()`。

### 4.8 2026-09-03 至 2026-09-04 — 文档阶段（无业务代码）

提交：

- `0ebf8eb982e96394fa67c9f83d75d57c87cc1d86`；
- `a41e76b9d58e18b6107742ec75e8f689d0b14390`；
- `3bed5ecf1353fe229aa0303494ca2188f6835dbe`。

**修改前**

- 代码能力、安全边界、验证结果和现场未决项分散在实现和对话中。

**修改后**

- 建立状态文档、现场清单和接手说明；
- 本次又将其压缩并按 `ORAN_version_UDP_V2` 重新建立时间线。

**好处**

- 代码提交、已验证事实和真机未验证事项可以分开核对；
- 这些提交只改文档，不应被当成新增运行功能。

### 4.9 2026-09-04 22:07 — 工程统一改名并使用仓库内 QCustomPlot

提交：`2ec49cf0c26c169b5eaf84c55b55995a18acc660`

**修改前**

- 解决方案、工程、UI/QRC、窗口类和输出仍叫 `test1`；
- V2 工程虽然仓库根目录已有 `qcustomplot.cpp/.h`，实际项目引用的却是仓库外 `..\QCustomPlot_File\...`；
- 只复制仓库可能因缺少外部 QCustomPlot 路径而无法编译；
- 旧名称容易让操作者误运行历史 `test1.exe`。

**修改后**

- `test1.slnx/.vcxproj/.filters/.cpp/.h/.ui/.qrc` 统一改为 `relay_station.*`；
- C++ 窗口类 `test1` 改为 `RelayStation`，UI 类改为 `RelayStationClass`；
- `main.cpp` 包含 `relay_station.h` 并创建 `RelayStation`；
- 输出目标固定为 `relay_station.exe`；
- 工程直接编译仓库根目录 `qcustomplot.cpp/.h`；
- 保存 QCustomPlot GPL 文本，历史 `test1_old.cpp` 同步改名但不作为活动主实现。

**好处**

- 项目、类、生成文件和 exe 名称一致，降低打开/运行错版本的概率；
- 当前分支源码不依赖上级 QCustomPlot 目录；
- 分支切换仍由 Git 恢复各分支自己的工程文件，未改写旧分支提交。

**关键文件**

- `main.cpp`；
- `relay_station.slnx/.vcxproj/.filters/.cpp/.h/.ui/.qrc`；
- `relay_station_old.cpp`；
- `third_party/qcustomplot/GPL.txt`。

### 4.10 2026-09-04 22:08 — 可重复构建和可搬运行目录

提交：`d91a03038203ffeeead14e72eedc9c584f305b26`

**修改前**

- 构建依赖本机 Visual Studio 会话和分散路径；
- 普通输出目录不保证同时存在 OpenCV DLL 和 `relay_config.json`；
- 没有一键收集 Qt 插件、MSVC CRT、OpenCV 和配置的部署入口；
- 第三方许可入口不集中。

**修改后**

- `scripts/build.ps1` 支持 Debug、Release 或 Both 的完整 Rebuild；
- `relay_station.vcxproj` 用 `OpenCvRoot` 解析本地完整 SDK、环境覆盖或本机后备路径；
- `CopyRuntimeDependencies` 在普通构建后复制对应 OpenCV DLL 和配置；
- `scripts/deploy.ps1` 调用 `windeployqt` 并归集 Qt、OpenCV、MSVC x64 CRT、配置和说明；
- 新增 `README.md`、`THIRD_PARTY_NOTICES.md`、OpenCV/FFmpeg/QCustomPlot 许可文本；
- `.gitignore` 排除本机构建和部署产物。

**好处**

- 构建和部署步骤可重复，遗漏 DLL 或配置时脚本会明确失败；
- `deploy\Release` 可作为运行目录复制；
- 源码依赖自包含与编译工具链安装边界被明确区分。

**关键文件/入口**

- `scripts/build.ps1`；
- `scripts/deploy.ps1`；
- `relay_station.vcxproj` 的 `OpenCvRoot` 和 `CopyRuntimeDependencies`；
- `README.md`、`THIRD_PARTY_NOTICES.md`、`third_party/*`。

### 4.11 2026-09-04 22:26 — 仓库根目录上移

提交：`1851e374ddfe1e9e046474cbb550a44b60fd519b`

**修改前**

- 仓库位于 `D:\ACN\Relay_Station_QT\test1`，多一层历史临时目录；
- 文档和命令示例使用旧绝对路径。

**修改后**

- 整个 Git 仓库连同 `.git` 原样上移到 `D:\ACN\Relay_Station_QT`；
- README 和状态文档命令更新为新根目录；
- 分支引用和提交历史未改变。

**好处**

- 源码、解决方案、脚本和部署目录直接位于正式目录；
- 压缩、复制和让另一台电脑上的 Codex 读取时不再多套一层 `test1`。

**关键文件**

- `README.md`；
- `docs/TouchVLA_Relay_Status.md`；
- 物理目录迁移本身不是 Git 内容差异。

### 4.12 2026-09-08 17:34 — EnableRobot 使用独立的 30 秒真实回复期限

提交：`c695d98446e9f37ec6d198c2ae446dddc28e5916`

**修改前**

- 8888 Legacy 与 8891 v2 创建 `PendingRequest` 时都固定使用 `kRobotReplyTimeoutNs=3000 ms`；
- `EnableRobot(...)` 与 `RobotMode()`、`MovL(...)` 等普通命令具有相同的 3 秒文字回复期限；
- 到期后中继立即向 Windows 报 `robot_reply_timeout`，随后到达的真实 CR3 回复会由 orphan debt 丢弃；
- 超时错误没有命令名和实际期限，现场难以判断是哪条命令以多长期限失败；
- Communication State 没有打印回复期限策略，无法从运行界面直接确认是否启动了新版本。

**现场证据**

```text
Windows 发送：
29999|EnableRobot(0.500,0.000,0.000,0.000)

中继先返回：
ERROR|robot_reply_timeout|late reply will be orphaned;

随后才收到真实 CR3 回复：
0,{},EnableRobot(...);

中继记录：
Robot 29999 orphan_reply discarded: 0,{},EnableRobot(...);
```

- 现场同时观察到 CR3 完成使能、指示灯变绿；绿灯只能作为现场现象，不能由中继据此生成或补造成功回复；
- 能证明的故障是“3 秒内未收到 CR3 的文字回复”，不能写成“CR3 拒绝使能”。

**发现的问题**

- CR3 的 `EnableRobot` 实际处理和回复可能超过 3 秒，固定期限会把“控制器稍后成功回复”误报成中继超时；
- 待请求一旦过期，真实迟到回复必须作为 orphan 处理，否则可能错配给后面的请求；所以不能简单取消超时或把迟到回复重新塞给 Windows；
- 必须延长的只有 `EnableRobot`，不能把普通 Dashboard、MovL 或其他路径一起改成 30 秒。

**修改后**

- 保留普通期限 `kRobotReplyTimeoutNs=3000 ms`，新增 `kEnableRobotReplyTimeoutNs=30000 ms`；
- 新增唯一选择函数 `robotReplyTimeoutNs(const QByteArray& command)`：命令名大小写不敏感地精确匹配 `EnableRobot` 时返回 30 秒，其余返回 3 秒；
- `routeLegacyCommand()` 与 `handleV2Command()` 都通过该函数设置 `PendingRequest::deadlineSteadyNs`，没有形成两套分叉逻辑；
- `ServoP` 的原有策略不变，仍不进入普通控制器回复等待；
- 30 秒是有限期限：仍未收到文字回复时照常登记 timeout/orphan，迟到回复不会错误匹配给新请求；
- Legacy 超时格式变为 `ERROR|robot_reply_timeout|command=EnableRobot,timeout_ms=30000,late reply will be orphaned;`；
- v2 的错误 `detail` 同时带 `command_name` 和 `timeout_ms`，并额外提供同名 JSON 字段；文字明确表示“期限内未收到控制器文字回复”，不表示 CR3 拒绝命令；
- `RelayRuntime::start()` 通过现有 `logMessage` 信号向 Communication State 打印：`>> Reply timeout policy: ordinary=3000 ms, EnableRobot=30000 ms`；
- 成功路径仍只原样转发真实 CR3 回复；没有伪造 `0,{5},RobotMode();` 或任何 EnableRobot 成功回复，也没有新增自动 `ClearError()`、自动 `EnableRobot()`、自动重发使能或旧动作恢复。

**好处**

- CR3 在 3 至 30 秒之间返回真实 `EnableRobot` 结果时，Windows 能收到并依据真实回复判断成功或失败；
- 普通命令的失败发现速度仍是 3 秒，不会因为使能特例整体变慢；
- 启动日志可直接识别新 exe 的期限策略，超时日志可直接看到命令名和实际等待时长；
- 保持严格的 FIFO/orphan 隔离，不用猜测绿灯状态，也不会把中继状态冒充控制器原始回复。

**生效场景**

- 对 TCP 8888 Legacy 和 TCP 8891 v2 两个入口提交的 `EnableRobot(...)` 都生效；大小写不同的 `enablerobot(...)` 同样选择 30 秒；
- `RobotMode()`、其他 Dashboard 命令和 `MovL(...)` 仍选择 3 秒；
- 本次不改变 30004 的 500 ms 状态新鲜度，不改变普通命令的执行/转发策略（仅补充超时诊断字段），不改变运动、ServoP、夹爪、视频、Force、Twin 或模型路径，不开启任何此前禁用的生产功能。

**测试结果**

- `tests/run_offline_tests.ps1 -Configuration Release`：`64/64 cases passed`；原 59 项半包、粘包、FIFO、orphan、Stop、8891、30004、视频、夹爪和重连测试全部继续通过；
- 新增 5 项：`EnableRobot` 30 秒、小写 `enablerobot` 30 秒、`RobotMode` 3 秒、`MovL` 3 秒、30 秒有限到期并将迟到回复登记为 orphan；
- 原运行时启动测试增加对期限策略日志全文的精确断言；
- `relay_station` Debug/Release 均完成 Rebuild，构建过程中没有启动中继程序，也没有连接或控制真实机器人。

**风险与回退**

- 指示灯变绿仍不能替代 CR3 文字回复；30 秒内没有真实回复时 Windows 不应被告知成功；
- 超过 30 秒仍会返回 `robot_reply_timeout`，更晚的真实回复仍作为 orphan 丢弃；这防止错配，但现场仍需核查控制器、网络与实际机器人状态；
- 同一 CR3 端口的回复按 FIFO 关联；等待中的 EnableRobot 会保留在该端口队首直至收到回复或到达 30 秒，期间不应并发发送依赖其结果的后续 Dashboard 命令；
- 回退本次功能应在 `vla_yj_relay` 上使用 `git revert c695d98446e9f37ec6d198c2ae446dddc28e5916`，然后重新执行 64 项测试并重建 Debug/Release；不得用 `git reset --hard` 覆盖现场文件。

## 5. 最终代码行为摘要

### 5.1 8888 命令和 CR3 回复

- 仅监听 TCP 8888，不监听 V2 分支中的 UDP 8888；
- 命令格式为 `29999|Command(...)` 或 `30003|Command(...)`；
- 支持 TCP 半包、粘包、嵌套括号、字符串和转义；
- 29999/30003 回复分别按 FIFO 排队；仅 `EnableRobot`（命令名大小写不敏感）使用 30 秒期限，其余等待回复的 Dashboard 和 MovL 命令使用 3 秒；
- 到期错误包含命令名和 `timeout_ms`；超时只表示期限内未收到 CR3 文字回复，30 秒后的 EnableRobot 迟到回复仍只能成为 orphan；
- CR3 原始回复原样返回，不由中继伪造成功；
- 旧 8888 的 MovL 等普通命令仍可能写入机器人；ServoP 被明确拒绝。

### 5.2 30004 状态和快照

- 帧长固定为 1440；
- `MessageSize` 必须为 1440；
- `TestValue` 必须为 `0x0123456789ABCDEF`；
- 必需 double 必须为有限值，布尔状态必须为 0/1；
- 半包、粘包和错位通过缓存与合法头搜索处理；
- 只有整帧通过才生成新的 `RobotStateSnapshot` 和递增 `state_seq`；
- UI、8891、Twin、Force 都读取同一整份快照；断线使旧快照失效。

### 5.3 8891 v2

- 帧格式：4 字节大端正文长度 + UTF-8 JSON；最大正文 1 MiB；
- 首帧必须是 `hello`，客户端必须为 `touchvla_windows`；
- 每条连接生成新 session UUID，普通 request ID 必须为非零十进制字符串；
- 主动推送 `robot_state`/`relay_status` 的 request ID 固定为 `"0"`；
- 慢客户端不会让状态快照无界排队；
- 非运动 command 可以关联真实 CR3 回复；
- v2 运动生产路径仍为 false，不会写 CR3。

### 5.4 Ready、Stop 和重连

- Ready 需要六个布尔条件全部为真：三个 CR3 端口分别连接、快照有效、连接代次非零、状态年龄不超过 500 ms；
- 200 ms 心跳持续发布状态；合法帧后 501 ms 未更新会触发 Not Ready；
- 三个 CR3 端口断线后每秒异步重连；
- 客户端替换、断开、Stop 清解析器、回复表、输出队列、状态、动作和 UDP 订阅；
- Stop 不自动发送 `ClearError()`、`EnableRobot()` 或 `DisableRobot()`，也不是实体急停。

### 5.5 Twin、Force、Video 和夹爪

- Twin 和 Force 使用同一合法快照；Force 四元数范数接近零时不发送可用数据；
- Twin 订阅超时 5 秒，Force 订阅超时 2 秒，Video 订阅超时 5 秒；
- 视频只保留最新一帧，旧 UDP 8886 单包格式保持兼容；
- TVF2 分片原型存在但生产禁用；
- 夹爪 installed/configured 均为 false，占位命令在写 CR3 前返回不可用错误。

## 6. 相比 V2 的文件覆盖表

以下是 `git diff --find-renames ORAN_version_UDP_V2 HEAD` 的完整范围归类。

| 类别 | 文件 | 净变化说明 |
|---|---|---|
| 协议核心 | `relay_core.cpp`、`relay_core.h` | 新增解析器、状态、动作门、视频队列和夹爪适配 |
| 运行时 | `relay_runtime.cpp`、`relay_runtime.h` | 新增 8888/8891/CR3 生命周期、回复关联、Ready、诊断 |
| UI 主实现 | `test1.cpp` → `relay_station.cpp` | 重命名并改为调用 RelayRuntime，保留兼容流和 UI |
| UI 头文件 | `test1.h` 删除、`relay_station.h` 新增 | 逻辑重命名且结构变化较大，Git 按删除/新增显示 |
| 程序入口 | `main.cpp` | 创建 `RelayStation` |
| Qt 资源/UI | `test1.ui`、`test1.qrc` → `relay_station.ui`、`relay_station.qrc` | 类名、包含和生成项统一改名 |
| 解决方案/工程 | `test1.slnx`、`test1.vcxproj`、`test1.vcxproj.filters` → `relay_station.slnx`、`relay_station.vcxproj`、`relay_station.vcxproj.filters` | 项目名、源文件、MOC/UIC/RCC、OpenCV 和运行依赖更新 |
| 历史文件 | `test1_old.cpp` → `relay_station_old.cpp` | 只改名保存历史，不作为当前核心实现 |
| 配置 | `relay_config.json` | 明确夹爪、v2 运动和 TVF2 禁用 |
| 测试 | `tests/relay_core_tests.cpp/.pro`、`run_offline_tests.ps1` | 64 项离线/回环测试和一键入口，包含命令级回复期限回归 |
| 构建部署 | `scripts/build.ps1`、`scripts/deploy.ps1` | 新增重复构建和运行包归集 |
| 说明 | `README.md`、`THIRD_PARTY_NOTICES.md` | 新增使用方法和依赖边界 |
| 许可 | `third_party/opencv/*`、`third_party/qcustomplot/GPL.txt` | 新增第三方许可文本 |
| 忽略规则 | `.gitignore` | 忽略 deploy 和本机构建产物 |
| 状态文档 | `docs/TouchVLA_Relay_Status.md` | 新增并在本次压缩为核心时间线 |

补充：`qcustomplot.cpp/.h` 的内容相对 V2 没有净变化，所以不出现在文件差异表；真正改变的是工程从引用仓库外副本改为编译这两个仓库内文件。

## 7. 验证证据

### 7.1 已完成

- 离线测试：`64/64 cases passed`；
- 覆盖 8888 半包/粘包、回复隔离、EnableRobot/普通命令期限、30 秒有限到期、30004 校验/重同步、session、Ready、动作 TTL、Stop、Twin/Force、latest-frame、夹爪拒绝和重连；
- `D:\ACN\Relay_Station_QT` 根目录下 Debug/Release 本次完整 Rebuild 均成功；
- 回环运行时测试确认启动信号包含 `>> Reply timeout policy: ordinary=3000 ms, EnableRobot=30000 ms`，该信号在正式 UI 中连接到 Communication State；
- 新根目录重建后的产物：

| 产物 | 大小 | SHA-256 |
|---|---:|---|
| `x64\Debug\relay_station.exe` | 5,980,160 B | `F14A3743B70422A8B7C8ADC005FBA16777D8E830B313443DA24893E384789E90` |
| `x64\Release\relay_station.exe` | 845,824 B | `53652F15BD049F6BE4A8B7ABA8D1D4ECB8A87C10423C7A63DD530DB9A550BC6E` |

- 迁移前的部署过程曾检查 Qt、OpenCV、MSVC CRT、配置和关键插件完整性，PE 非系统依赖缺失数为 0；
- 所有上述构建、测试和部署检查均未启动主程序、未连接 CR3。

### 7.2 当前可执行文件注意事项

- 当前工程目标名只能是 `relay_station.exe`；
- 2026-09-05 只读检查发现 `x64\Debug\test1.exe`，SHA-256 为 `9979A799B997BD1896EBE05D36646927A1159D7ABFEF20FEF1A28E1CBCD492A2`；它不是当前工程目标，不得当成当前版本运行；
- `deploy\Release\relay_station.exe` 是迁移前生成的包，其哈希 `D3CC0A4E851AD4D6AD9657558F48A0B399C1469F8C6EB8056ADC74147243CCF3` 与新根目录最新 Release 哈希不同；
- 因此正式复制到上位机前，应重新执行 deploy，随后记录 HEAD、exe 路径、大小、时间和 SHA-256；本次已重建工程输出但未重新生成部署包，也未启动程序。

## 8. 尚未完成的真机验证

| 项目 | 当前状态 | 上机前要求 |
|---|---|---|
| 当前 exe 来源 | 本次 Debug/Release 构建结果已记录，但部署包仍可能是旧版本 | 重新部署并记录 HEAD/哈希，现场核对启动期限策略日志 |
| 30004 布局/单位/频率 | 只用官方 V3 布局和合成帧验证 | 对真实固件核对 1440、字段偏移、单位和约 125 Hz |
| 29999/30003/30004 重连 | 本机回环验证 | 做断电、拔线、恢复和迟到回复测试 |
| Windows 8891 | 协议和回环已验证 | 与真实 `ReliableRelayClient` 联机验证握手、状态和 session |
| Windows 8888 | parser/回环已验证 | 验证真实半包、粘包、同名迟到回复和断线感知 |
| MovL 松手和旧动作 | v2 动作门离线验证，旧 8888 仍是生产兼容路径 | 验证 Windows 松手后不会继续发送/执行旧目标 |
| ServoP | 生产禁用 | 完成周期、抖动、报警、停止和机械风险评审后才可考虑启用 |
| Stop/急停 | 只验证软件清理 | 实测 DisableRobot 回复、制动距离和实体急停 |
| Force | 同帧与有限值离线验证 | 核对坐标系、方向、单位、四元数顺序和工具负载影响 |
| 视频 | latest-frame/元数据离线验证 | 实测帧率、端到端延迟、UDP 丢包、JPG 大小和 30 分钟稳定性 |
| 夹爪 | 未安装、未配置、命令拒绝 | 确定型号、接口、协议、范围、反馈、负载和重心 |
| TCP/User/Tool/负载/重心 | 当前程序不自动核对真实机器人配置 | 从 CR3/上位机读取并与机械安装实测值交叉确认 |
| 安全空间 | 未真机验证 | 完成限速、工作空间、障碍物、停止距离和人工急停值守 |

## 9. 当前不可突破的安全边界

1. 不把 TCP/UDP 连接成功当作机器人可安全运动；
2. 不把 `write()` 成功当作 CR3 已执行或机器人已到达；
3. 不自动调用 `ClearError()`、`EnableRobot()` 或 `DisableRobot()`；
4. 不启用 v2 运动、ServoP、TVF2 或未配置夹爪；
5. 不把网络 Stop 当作实体急停；
6. 不凭 UI 默认值假定 TCP、User、Tool、负载、重心和坐标系正确；
7. 未完成真实 Windows/CR3/机械验收前，离线测试只能证明软件逻辑，不证明现场安全。
8. 机械臂绿灯只能作为现场现象；没有真实 CR3 文字回复时，中继不得生成成功回复。

## 10. 提交时间线

| 时间 | 提交 | 作用 |
|---|---|---|
| 2026-04-12 13:19 | `661de3c57049a46655daf68eb04e30d04ab701d5` | 对比基准：ORAN_version_UDP_V2 的 TCP/UDP 8888 路线 |
| 2026-05-26 16:56 | `8420d66eb4a1211f97a38ec0d648a26ebe8457d1` | 当前路线起点：Force 到 Touch、TCP 8888 路线 |
| 2026-09-03 21:13 | `1ee0a908064721ed202179b564d3545396d77f69` | 协议核心和 30004 离线夹具 |
| 2026-09-03 21:34 | `5142d19454a219d2794277699e71945c3b2ea316` | 验证状态、8891 v2 session 和 RelayRuntime |
| 2026-09-03 21:38 | `8996555b9f98293141440dbaf4901d2ea21dce60` | Stop/重连隔离、UI 接入和兼容流 |
| 2026-09-03 21:45 | `1f03d422241bb86cf43a9c5b58d0bfeaffc5270b` | 有界流诊断和相机元数据 |
| 2026-09-03 21:46 | `2a19e279e613b2e648ceda8daf7a2344892da8d4` | 兼容与失败路径测试、一键测试脚本 |
| 2026-09-03 22:06 | `d187ef252b0d7baa5abb9ec89425e453dbcd5522` | 生命周期和错误分类诊断 |
| 2026-09-03 22:11 | `0ebf8eb982e96394fa67c9f83d75d57c87cc1d86` | 初始状态与现场清单文档 |
| 2026-09-03 22:14 | `a41e76b9d58e18b6107742ec75e8f689d0b14390` | 文档格式整理 |
| 2026-09-04 21:52 | `3bed5ecf1353fe229aa0303494ca2188f6835dbe` | 接手流程和诊断说明 |
| 2026-09-04 22:07 | `2ec49cf0c26c169b5eaf84c55b55995a18acc660` | 项目改名和 QCustomPlot 内部引用 |
| 2026-09-04 22:08 | `d91a03038203ffeeead14e72eedc9c584f305b26` | 可重复构建和便携部署 |
| 2026-09-04 22:26 | `1851e374ddfe1e9e046474cbb550a44b60fd519b` | 仓库根路径更新 |
| 2026-09-05 11:20 | `3363331459a70f9d344eb5aa3fba6ba3442f71f1` | 压缩状态文档并按 ORAN_version_UDP_V2 重建完整修改时间线 |
| 2026-09-08 17:34 | `c695d98446e9f37ec6d198c2ae446dddc28e5916` | EnableRobot 30 秒命令级回复期限、启动可见性、超时诊断和回归测试 |

回退应优先在当前分支按逆序使用 `git revert <hash>`，不得用 `git reset --hard` 覆盖现场工作区。
