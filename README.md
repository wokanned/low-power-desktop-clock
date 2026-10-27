# DeskClock 桌面时钟

更新日期：2026-10-27。

基于 STM32L471RET6、FreeRTOS 和 GDEY029T94-FL03 墨水屏的桌面时钟工程。当前已接入 RTC 日历、黑白时钟与天气页、ADXL345 遥测、W25Q128 时制配置，以及 USART2 天气注入和整秒校时。

## 从这里开始

- [文档导航](docs/README.md)：按使用场景查找说明。
- [天气串口格式](docs/weather_protocol.md)：九个字段、天气与风向编码、发送示例、切页和过期规则。
- [串口校时格式](docs/time_protocol.md)：北京时间 TIME 命令、结果应答和故障恢复。
- [调试控制台](docs/debug_console.md)：JustFloat、CONSOLE、FORMAT 和 ERRORS。
- [当前进度与后续计划](docs/totalPLAN.md#当前开发状态2026-10-27)：已实现功能、验证状态和待办。

## 构建与接线

使用 Keil MDK-ARM / ARMCC 打开 `MDK-ARM/DeskClock.uvprojx` 构建。当前没有 CMake 整机工程；主机 GCC 测试入口位于 `tests/`。开发前阅读 [AGENTS.md](AGENTS.md) 和[嵌软开发规范](docs/嵌软开发规范.md)。

USART2 为 H10 调试口，115200、8N1、3.3V TTL。发送端 TX 接 STM32 PA3，RX 接 PA2，共地。板载 ESP32-C3 接 USART1，尚未接入联网业务；使用外部 ESP32 向 USART2 发报文时需连接 H10。

默认串口输出 JustFloat 二进制遥测。WEATHER、PAGE、TIME 可在默认模式静默接收；需要文本结果时先发送 `CONSOLE ON`。详细格式以各协议文档为准。

## 最近验证

2026-10-27 天气与 TIME 集成后的相关主机测试通过，默认黑白完整 Keil 构建为0错误、0警告。镜像、哈希和验证范围见[测试数据与验证记录](docs/测试数据.md)。尚未烧录该镜像，整机板测待完成。

ESP32 自动联网与 NTP、环境及电量采集、按键响铃、整机低功耗仍在后续计划中。
