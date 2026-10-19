# USART2 显式文本控制台

USART2 使用 H10，PA2 TX / PA3 RX，115200、8N1、3.3V TTL。默认发送现有
16 通道、68 字节 JustFloat 帧，`debugTask` 始终是唯一发送者。

## 命令

命令区分大小写，不带前后空格，以 CR、LF 或 CRLF 结束。每行内容最多
31 字节。二进制模式只接受 `CONSOLE ON`，其他命令不发送文本响应。

| 命令 | 结果 |
| --- | --- |
| `CONSOLE ON` | 完整遥测帧发送结束后清空旧队列，暂停遥测，回复 `OK CONSOLE ON` |
| `FORMAT 12` / `FORMAT 24` | 异步请求保存格式；验证持久化成功后回复 `OK FORMAT 12` / `OK FORMAT 24` |
| `ERRORS` | 输出活动类别数、故障修订号、保留 EPD 状态；各来源输出当前原因、初次原因、首次完整时间和 tick |
| `CONSOLE OFF` | 回复 `OK CONSOLE OFF` 成功后清空旧队列，恢复二进制输出 |

存储请求尚未完成时，新的 FORMAT 或 CONSOLE OFF 返回 `ERR BUSY`；ERRORS
仍可使用。存储未就绪、拒绝请求或提交失败返回 `ERR STORAGE`。未知文本命令
返回 `ERR COMMAND`，损坏或超长命令整行丢弃并返回 `ERR RX`。此版本不实现
校时、NTP 或任意 Flash 读写命令。

`ERRORS` 的 active/first 是十六进制位掩码，依次为 IO=001、CONFIG=002、
TIMEOUT=004、ID=008、VERIFY=010、START=020、TX=040、RX=080、OVERFLOW=100。
已恢复来源仍可显示上次故障的首次记录，active 为 000；没有可靠发生时间时
显示 `--:--`。EPD 状态直接来自 `display_get_last_status()`，不作为屏幕错误类别。

## 发送与接收约束

控制台期间加速度采样继续，`Debug_SendFloats()` 成功返回且不入队。
`Debug_GetSuppressedCount()` 保留主动暂停次数，不增加队列拒绝计数，
也不因主动暂停产生 SYSTEM 错误。退出后只发送新入队的数据。

文本仅由 debugTask 分块发送，每块最多 64 字节，单次 HAL 超时 15 ms。
失败的部分发送不重放。CONSOLE OFF 应答失败时保持控制台状态，避免紧接部分
文本发送二进制帧。电脑应停止 VOFA 捕获后操作控制台，再重新开始一次捕获。

USART2 IRQ 优先级为 5。`.ioc` 启用 IRQ，由 CubeMX 在
`Core/Src/stm32l4xx_it.c` 生成唯一中断入口并调用 `HAL_UART_IRQHandler(&huart2)`。
平台仅持有 HAL RX 和错误回调。重新生成后应检查中断入口唯一及 Keil 源文件分组。
ISR 只收单字节、写 256 字节静态环形缓冲并通知 debugTask。任务解析命令，
20 ms 内轮询异步完成。环形缓冲溢出时清空待处理字节，丢弃当前行到下一个
换行；不会将损坏行的后半段当作新命令执行。

HAL 错误在重新接收前保留，任务只调用接收端恢复，不中止 TX。当前 TX/RX/溢出
故障通过 DEBUG 来源归入 SYSTEM；成功发送或完整有效命令分别清除相应故障。
累计错误计数不清除。UART 超时不是完整往返命令延迟的实测上限。

## 验证

运行 `./tests/run_debug_host_tests.ps1`，覆盖精确命令和边界、CR/LF、整行丢弃、
二进制与文本互斥、异步持久化应答、忙拒绝、全部故障时间输出、主动暂停、
失败的退出应答、环形缓冲溢出、HAL 错误保存和 RX 单独恢复。
原遥测回归入口为 `./tests/run_adxl345_host_tests.ps1`。

上述测试使用主机桩，不证明实机 IRQ、RTOS 调度、栈余量或串口电气性能。
板上需确认持续遥测、进入/退出控制台、保存重启后的格式，以及故障后接收恢复。
