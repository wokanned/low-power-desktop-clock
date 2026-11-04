# CLAUDE.md

本文件是本工程的 AI 文档入口。仓库规则集中维护在 [AGENTS.md](AGENTS.md)，避免多份规则和进度副本逐渐失步。处理任务前先阅读该文件及本次任务对应的规范。

更新日期：2026-11-04。使用说明统一见[文档导航](docs/README.md)。面向使用者的指导文档使用中文，命令、API、路径和原始日志保留原文。

## 工程状态入口

- 当前进度、上电预期和下一步见 [技术方案](docs/totalPLAN.md) 的“当前开发状态（2026-11-04）”。
- 当前为软件集成原型，功能清单统一见技术方案；下方2026-10-27验证条目仅描述当时镜像。
- 2026-10-27 天气与 TIME 集成后的主机测试及默认黑白完整 Keil 构建通过，构建身份、镜像哈希与范围见[验证记录](docs/测试数据.md)。该镜像未烧录，整机板测待完成。旧版本的成功记录不自动适用于当前工作区。
- ESP32 自动联网/NTP、环境与电量采集、按键闹钟、分域电源调度仍待实现。MCU Tickless/STOP2、Activity 动作唤醒、WDATA 多页预警及 TIME2/TIME? 已接入，详见当前进度与[已知差异](docs/current_code_gaps.md)。

默认 AUTO 不连续遥测；串口操作先在清醒窗口取得 `OK POWER HOLD`，文本操作再进入控制台，完成后 `POWER AUTO`。CONSOLE 开关不控制待机。

## 开发与验证入口

- 当前唯一整机构建入口为 `MDK-ARM/DeskClock.uvprojx`，使用 Keil MDK-ARM / ARMCC。新增或移动源码时同步工程分组、include 路径及调用入口。
- 当前没有 CMake 工程。`tests/*.ps1` 中的主机 GCC 测试用于辅助验证，不能替代目标固件链接、下载和实板验证。
- CubeMX 维护 `.ioc` 和生成代码；`Core/` 中用户修改限于 `USER CODE BEGIN/END`。业务放在 `app/`，器件协议放在 `device/`，HAL 绑定放在 `platform/`，遵守 AGENTS.md 的依赖方向和编码规则。
- 显示控制器、帧缓冲和 Paint 画布由唯一显示任务独占。生产默认 `EPD_VENDOR_DEMO_MODE=0, EPD_CLOCK_USE_GRAY4=0`，正常时钟按分钟局刷时间/内存格，切页、故障及内容变化等触发全刷。
- 分开记录源码接入、主机验证、整机构建、下载和硬件验收；没有执行的步骤写明待验证。文档改动只需核对内容与引用，不要求为此编译或操作硬件。

## 按任务阅读

- [嵌软开发规范](docs/嵌软开发规范.md)：所有非简单任务的分层、生成代码和构建规则。
- [IC 开发规范](docs/ic开发规范.md)：新增器件、寄存器、初始化时序、总线与恢复逻辑。
- [显示移植记录](docs/epd_gdey029t94_port.md)、[时钟界面](docs/clock_gray4_ui.md)：显示引脚、刷新、故障和验证记录。
- [串口控制台](docs/debug_console.md)、[天气协议](docs/weather_protocol.md)、[校时协议](docs/time_protocol.md)：观测、时制、天气注入、切页与 TIME 校时。
- [ADXL345 遥测](docs/adxl345_telemetry.md)、[W25Q128 存储](docs/w25q128_storage.md)：采样与存储模块的具体证据和未验收项。
- [遥测与实验调试工作流](docs/嵌入式遥测与实验调试工作流.md)：串口实验、日志和板上观测。
