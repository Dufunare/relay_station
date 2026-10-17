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

## 安全边界

## 兼容 TouchControlPanelSolution 的 ServoP 运动转发

现有上位机通过 TCP 8888 发送以换行结尾的文本命令，例如：

```text
30003|ServoP(250.000,100.000,200.000,150,0,90)
```

中转站的 Legacy 解析器会识别 `30003|` 目标端口前缀与命令括号，将 **原样 `ServoP(...)`（不含前缀、换行）** 写入已连接的机械臂 TCP 30003。支持 TCP 半包和连续多条命令；`ServoP` 不进入普通命令回复等待队列，也不会由中转站伪造执行成功回复。端口 29999 的 `ServoP` 会被拒绝；机械臂 30003 未连接或发送队列满时仍返回原有错误。

**范围：只启用 Legacy TCP 8888 的 ServoP 转发，TCP 8891 v2 运动总闸仍关闭。** 中转站目前不验证目标工作空间、机械臂运动模式或现场安全条件，真机操作前必须先由上位机和现场人员完成安全检查，确保实体急停可用。若暂不准备真机测试，请使用本地模拟运动 TCP 服务端运行离线测试。

运行程序不等于允许机器人安全运动。旧版 TCP 8888 已允许 `ServoP` 实时运动指令转发，**连接到真实机器人时会产生运动**；v2 运动、TVF2 和未配置夹爪仍保持生产禁用。网络 Stop 不能代替实体急停。真机操作前请阅读 `docs\TouchVLA_Relay_Status.md` 的现场验证清单。
