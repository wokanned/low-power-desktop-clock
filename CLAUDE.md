# CLAUDE.md

本文件是本工程的 AI 文档入口。仓库规则集中维护在 [AGENTS.md](AGENTS.md)，避免多份规则和进度副本逐渐失步。处理任务前先阅读该文件及本次任务对应的规范。

## 工程状态入口

- 当前进度、上电预期和下一步见 [技术方案](docs/totalPLAN.md) 的“当前开发状态（2026-10-27）”。
- 当前是软件集成原型：RTC/时钟显示、ADXL345 遥测、W25Q128 配置存储、USART2 天气注入已接入。
- 最新天气集成已有主机测试记录，当前完整版本的 Keil 构建、镜像身份和整机板测仍待完成。旧版本的成功记录不自动适用于当前工作区。
- ESP32 自动联网/NTP、环境与电量采集、按键闹钟、整机低功耗仍待实现。

## 开发与验证入口

- 当前唯一整机构建入口为 `MDK-ARM/DeskClock.uvprojx`，使用 Keil MDK-ARM / ARMCC。新增或移动源码时同步工程分组、include 路径及调用入口。
- 当前没有 CMake 工程。`tests/*.ps1` 中的主机 GCC 测试用于辅助验证，不能替代目标固件链接、下载和实板验证。
- CubeMX 维护 `.ioc` 和生成代码；`Core/` 中用户修改限于 `USER CODE BEGIN/END`。业务放在 `app/`，器件协议放在 `device/`，HAL 绑定放在 `platform/`，遵守 AGENTS.md 的依赖方向和编码规则。
- 显示控制器、帧缓冲和 Paint 画布由唯一显示任务独占。生产默认 `EPD_VENDOR_DEMO_MODE=0, EPD_CLOCK_USE_GRAY4=0`，正常时钟按分钟黑白全刷。
- 分开记录源码接入、主机验证、整机构建、下载和硬件验收；没有执行的步骤写明待验证。文档改动只需核对内容与引用，不要求为此编译或操作硬件。

## 按任务阅读

- [嵌软开发规范](docs/嵌软开发规范.md)：所有非简单任务的分层、生成代码和构建规则。
- [IC 开发规范](docs/ic开发规范.md)：新增器件、寄存器、初始化时序、总线与恢复逻辑。
- [显示移植记录](docs/epd_gdey029t94_port.md)、[时钟界面](docs/clock_gray4_ui.md)：显示引脚、刷新、故障和验证记录。
- [串口控制台](docs/debug_console.md)、[天气协议](docs/weather_protocol.md)：观测、时制、天气注入及切页。
- [ADXL345 遥测](docs/adxl345_telemetry.md)、[W25Q128 存储](docs/w25q128_storage.md)：采样与存储模块的具体证据和未验收项。
- [遥测与实验调试工作流](docs/嵌入式遥测与实验调试工作流.md)：串口实验、日志和板上观测。
