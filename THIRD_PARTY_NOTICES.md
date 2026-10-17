# Third-party notices

本项目使用以下第三方组件。本文只做来源和许可文件索引，不替代各组件的正式许可文本或项目持有者的商业许可判断。

## QCustomPlot 2.1.1

- 项目主页：<https://www.qcustomplot.com/>
- 实际编译源码：仓库根目录 `qcustomplot.cpp`、`qcustomplot.h`
- 随附许可文本：`third_party/qcustomplot/GPL.txt`

QCustomPlot 提供 GPL 和商业许可选项。分发或闭源使用时应由项目负责人确认当前项目采用的许可依据。

## OpenCV 4.12

- 项目主页：<https://opencv.org/>
- 许可文本：`third_party/opencv/LICENSE.txt`
- 随安装包附带的 FFmpeg 许可说明：`third_party/opencv/LICENSE_FFMPEG.txt`

OpenCV SDK 仍作为编译环境安装；部署脚本仅将当前配置所需的运行 DLL 复制到本目录下的 `deploy` 输出。

## Qt 6.11

- 项目主页：<https://www.qt.io/>

Qt SDK 和 QtMsBuild 仍作为本机编译工具链安装。`windeployqt` 会根据本机安装把运行 DLL/插件复制到 `deploy`。部署和分发前必须按本项目实际 Qt 商业或开源许可完成合规检查。
