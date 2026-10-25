# Relay Station

该仓库现在直接位于 `D:\ACN\Relay_Station_QT`，项目名、窗口类和输出程序统一为 `relay_station` / `RelayStation`。

## 打开项目

Visual Studio 打开：

```text
relay_station.slnx
```

主项目文件：

```text
relay_station.vcxproj
```

## 一键构建

```powershell
Set-Location 'D:\ACN\Relay_Station_QT'
& .\scripts\build.ps1 -Configuration Both
```

输出：

```text
x64\Debug\relay_station.exe
x64\Release\relay_station.exe
```

普通构建会把对应的 OpenCV DLL 和 `relay_config.json` 复制到 exe 目录。Visual Studio 中选择 `Debug|x64` 或 `Release|x64` 后也可以直接构建/运行。

## 生成可运行目录

```powershell
& .\scripts\deploy.ps1 -Configuration Release
```

脚本会在当前仓库目录内生成：

```text
deploy\Release\
```

其中包含 `relay_station.exe`、OpenCV DLL、Qt DLL/插件、MSVC x64 运行库、`relay_config.json` 和第三方说明。部署脚本会逐项检查关键运行文件；它只打包，不自动启动程序，也不连接 CR3。

## 依赖边界

项目源码依赖已经自包含：

- QCustomPlot 的实际编译版本就是仓库根目录的 `qcustomplot.cpp/h`；
- QCustomPlot 与 OpenCV 的许可文本保存在 `third_party`；
- 工程不再引用 `..\QCustomPlot_File`。

编译工具链仍需安装在电脑上：

- Visual Studio/MSVC `v145` 与 Windows SDK；
- Qt 6.11 MSVC x64 SDK 和 Qt Visual Studio/MSBuild 集成；
- OpenCV 4.12 MSVC SDK。

这些是编译器/SDK，不属于项目源码，不能用复制少量文件的方式代替安装。脚本支持使用环境变量覆盖本机路径：

```text
MSBUILD_PATH
QT_MSBUILD
QT_ROOT
OPENCV_ROOT
VS_DEV_SHELL
```

如果把完整 OpenCV SDK 放到 `third_party\opencv`，工程会优先使用它；默认仍兼容本机 `D:\APP\OpenCV\opencv`。

## 离线协议测试

```powershell
& .\tests\run_offline_tests.ps1 -Configuration Release
```

测试使用本机回环和合成数据，不启动主程序、不连接或移动真实机器人。

## 视频视觉处理

收到 UDP 8886 的 `VIDEO_START` 后，摄像头采集线程把最新原始帧交给独立的
OpenCV 处理线程。处理后的 JPEG 会直接叠加以下可视信息，再按原有 UDP 单包协议发送：

- 主要角点和最长线段；
- 线段主方向、画面中心标记；
- 亮度、对比度、清晰度、边缘密度和处理帧率。

原始帧和编码帧队列都只保留最新一帧。处理速度不足或网络发送变慢时会丢弃旧帧，
不会让视频延迟持续累积。为控制单个 UDP 数据报大小，JPEG 会在需要时自动降低质量。

## 安全边界

运行程序不等于允许机器人运动。当前 **Legacy 8888 → 30003 的 ServoP 已恢复转发**（仅供明确授权、现场有物理急停的 Touch 遥控）；ServoP 按在线流处理，不等待普通应答。v2 运动、TVF2 和未配置夹爪仍保持生产禁用。网络 Stop/ResetRobot 不等于物理急停，现场控制端必须对按键释放、失联、机器人反馈失效等边界作处理。真机操作前请阅读 `docs\TouchVLA_Relay_Status.md` 的现场验证清单。
