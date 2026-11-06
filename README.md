# DeskClock 桌面时钟

更新日期：2026-11-04。

基于 STM32L471RET6、FreeRTOS 和 GDEY029T94-FL03 墨水屏的桌面时钟工程。当前已接入 RTC 日历、黑白时钟与天气页、ADXL345 遥测、W25Q128 时制配置，以及 USART2 天气注入和整秒校时。默认黑白连续显示在时钟页对时间和内存格采用局刷，旧天气页头局刷，WDATA天气页快照变化全刷，切页仍全刷。有 WDATA 时为时钟180秒→实况60秒→三日60秒→六格60秒，未收到 WDATA 时沿用时钟/旧天气各180秒。

## 从这里开始

- [文档导航](docs/README.md)：按使用场景查找说明。
- [天气串口格式](docs/weather_protocol.md)：九个字段、天气与风向编码、发送示例、切页和过期规则。
- [多天预报与断电保存](docs/forecast_protocol.md)：先校时、按完整日期接收1～7天预报、批次查询和外部Flash恢复。
- [串口校时格式](docs/time_protocol.md)：北京时间 TIME 命令、结果应答和故障恢复。
- [四键浏览与接口](docs/buttons.md)：上页、下页、回时钟、暂停／恢复轮播及首次按键唤醒。
- [调试控制台](docs/debug_console.md)：JustFloat、CONSOLE、FORMAT 和 ERRORS。
- [当前进度与后续计划](docs/totalPLAN.md#当前开发状态2026-11-04)：已实现功能、验证状态和待办。

## 构建与接线

使用 Keil MDK-ARM / ARMCC 打开 `MDK-ARM/DeskClock.uvprojx` 构建。当前没有 CMake 整机工程；主机 GCC 测试入口位于 `tests/`。开发前阅读 [AGENTS.md](AGENTS.md) 和[嵌软开发规范](docs/嵌软开发规范.md)。

USART2 为 H10 调试口，115200、8N1、3.3V TTL。发送端 TX 接 STM32 PA3，RX 接 PA2，共地。板载 ESP32-C3 接 USART1，尚未接入联网业务；使用外部 ESP32 向 USART2 发报文时需连接 H10。

默认 AUTO 自动待机，JustFloat 为默认串口格式但不持续发送遥测；在上电30秒或动作10秒清醒窗口先发送 `POWER HOLD` 并等待 `OK POWER HOLD`，需要文本应答再发送 `CONSOLE ON`。HOLD 且退出控制台后才持续输出遥测。WEATHER、PAGE、TIME 可在默认模式静默接收；需要文本结果时先发送 `CONSOLE ON`。详细格式以各协议文档为准。

## 最近验证

2026-11-04 四键浏览与按键唤醒已通过7组相关主机测试，默认黑白Keil完整重建0错误/0警告；尚未烧录或板测。精确构建、主机回归及板测状态见[测试数据与验证记录](docs/测试数据.md)。此前 SPI2 DMA 等构建记录只对应其归档输入，不替代局刷版本的验证。

MCU Tickless/STOP2、ADXL345 Activity 动作唤醒、WDATA 多页天气/预警和 TIME2/TIME? 已接入。ESP32 自动联网/NTP、Python 天气发送器、环境及电量采集、闹钟响铃、分域断电仍待实现；四键浏览与按键唤醒已接入，待实板验收。串口首包自动唤醒未实现，暂按先动作唤醒再连接使用。

B1～B5的修复/验证状态及另外5项已确认但未实现的业务调整见[代码与业务差异清单](docs/current_code_gaps.md)。
