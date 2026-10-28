# USART2 显式文本控制台

更新日期：2026-10-28。文档入口见 [文档导航](README.md)，工程总进度见 [totalPLAN.md 的“当前开发状态（2026-10-27）”](totalPLAN.md#当前开发状态2026-10-27)。控制台、天气注入、切页和 TIME 校时已接入。USART2 TX 已改为 DMA，最新构建与验证范围见[验证记录](测试数据.md)；最新镜像尚未烧录，板上验收仍待完成。

USART2 使用 H10，PA2 TX / PA3 RX，115200、8N1、3.3V TTL。默认发送现有
16 通道、68 字节 JustFloat 帧，`debugTask` 始终是唯一发送者。
普通文本终端看到二进制乱码符合默认协议。`Debug_Log()` 当前为空实现，启动、重试和
挂起位置的文字不会自动输出；发送 `CONSOLE ON` 并换行后才使用下文文本应答。

## 命令

命令区分大小写，不带前后空格，以CR、LF或CRLF结束。WEATHER行最多127字节，FS行最多223字节；其他输入保留31字节限制。长度不含换行和终止NUL。
只有精确WEATHER命令族（后接空格或逗号）和FS命令族使用各自长行上限，WEATHERX等未知命令仍受31字节限制。
二进制模式下 `WEATHER`、`PAGE` 和 `TIME` 命令静默执行，其他文本仅
`CONSOLE ON` 会产生切换应答。

| 命令 | 结果 |
| --- | --- |
| `CONSOLE ON` | 完整遥测帧发送结束后清空旧队列，暂停遥测，回复 `OK CONSOLE ON` |
| `FORMAT 12` / `FORMAT 24` | 异步请求保存格式；验证持久化成功后回复 `OK FORMAT 12` / `OK FORMAT 24` |
| `ERRORS` | 输出活动类别数、故障修订号、保留 EPD 状态；各来源输出当前原因、初次原因、首次完整时间和 tick |
| `CONSOLE OFF` | 回复 `OK CONSOLE OFF` 成功后清空旧队列，恢复二进制输出 |
| `WEATHER <record>` | 控制台回复 `OK WEATHER`；完整记录校验成功后原子替换 RAM 天气缓存、选择天气页并重启三分钟轮换计时 |
| `PAGE WEATHER` / `PAGE CLOCK` | 控制台分别回复 `OK PAGE WEATHER` / `OK PAGE CLOCK`；手动选择目标页面并重启三分钟计时，状态仅保存在 RAM |
| `TIME YYYY-MM-DDTHH:MM:SS` | 北京时间直接校时；RTC 写入并读回验证成功后，控制台回复 `OK TIME` |
| `FS STATUS` / `FS LIST /` | 查询文件系统快照或目录；STATUS 的 FLASH 字段保留文件故障的底层原因，文件检查中返回未就绪 |
| `FS MKDIR /name` / `FS REMOVE /name` / `FS RENAME /old /new` | 异步文件维护 |
| `FS FORMAT CONFIRM` / `FS SELFTEST CONFIRM` | 分别格式化文件分区或擦写专用自检扇区，上传期间拒绝 |
| `FS TRANSFER` | 进入带CRC32的二进制文件维护会话；详细协议及Python工具见[文件系统](filesystem.md) |

存储请求尚未完成时，新的 FORMAT 或 CONSOLE OFF 返回 `ERR BUSY`；ERRORS
仍可使用。WEATHER、PAGE 和 TIME 命令不受存储事务阻塞。校时在途时再次 TIME 或 CONSOLE OFF 返回 `ERR BUSY`，FORMAT 仍可提交。TIME 格式错误返回 `ERR TIME`，时钟任务不可用或 RTC 操作失败返回 `ERR RTC`。天气内容错误返回
`ERR WEATHER`，损坏或超长命令整行丢弃并返回 `ERR RX`。存储未就绪、拒绝请求
或提交失败返回 `ERR STORAGE`。未知文本命令返回 `ERR COMMAND`。NTP 和任意 Flash 读写命令尚未实现。校时格式及运行行为见 [time_protocol.md](time_protocol.md)，天气字段和有效范围见 [weather_protocol.md](weather_protocol.md)。
启动默认选择时钟页，此后每三分钟在时钟页和天气页之间切换。每个完整有效 `WEATHER`
包会选择天气页、重启三分钟计时，重复有效包也一样；无效天气或接收错误不重置。
手动 `PAGE` 命令也从该命令重新计时，包括重复选择当前页。天气、页面和计时不跨复位保存，
格式配置则由 storageTask 持久化。轮换使用单调 tick，`TIME` 校时和 `CONSOLE ON/OFF`
不改变周期。故障覆盖期间继续更新最终目标，BUSY 期间合并最新画面。详细规则见[天气协议](weather_protocol.md)。
`OK WEATHER` 和 `OK PAGE ...` 表示缓存或页面选择已接受；`OK FORMAT ...` 表示格式已验证保存；`OK TIME` 表示 RTC
写入、同步及读回验证已完成。这些应答均不表示屏幕刷新已经完成。

`ERRORS` 的 active/first 是十六进制位掩码，依次为 IO=001、CONFIG=002、
TIMEOUT=004、ID=008、VERIFY=010、START=020、TX=040、RX=080、OVERFLOW=100。
已恢复来源仍可显示上次故障的首次记录，active 为 000；没有可靠发生时间时
显示 `--:--`。EPD 状态直接来自 `display_get_last_status()`，不作为屏幕错误类别。

## 发送与接收约束

控制台期间加速度采样继续，`Debug_SendFloats()` 成功返回且不入队。
`Debug_GetSuppressedCount()` 保留主动暂停次数，不增加队列拒绝计数，
也不因主动暂停产生 SYSTEM 错误。退出后只发送新入队的数据。

文本和文件应答仅由 debugTask 分块发送，每块最多 64 字节；JustFloat 保持整帧 68 字节。
平台调用 `HAL_UART_Transmit_DMA()`，以独立的静态二进制信号量等待 UART TC，等待参数仍为 15 ms。
等待期间任务让出 CPU，RX 和队列使用的任务通知保留给 debugTask 原有循环。
发送接口保持同步返回，只有最后一字节发送完成才返回成功，调用者的缓冲区一直有效到函数返回。
超时、DMA 错误或启动失败时仅中止 TX，返回前停止 DMA，不自动重放。清理在短临界区内完成，
防止 DMA 完成中断在中止过程中重新打开 UART TC 中断。ISR、调度器未运行或中断被屏蔽时拒绝发送。
CONSOLE OFF 应答失败时保持控制台状态，避免紧接部分
文本发送二进制帧。电脑应停止 VOFA 捕获后操作控制台，再重新开始一次捕获。

TX 使用 DMA1 Channel7、Request2、普通模式、字节传输和内存自增，RX 仍为单字节中断。
USART2 与 DMA1 Channel7 IRQ 优先级均为 5，可调用 FreeRTOS 的 ISR API。
`.ioc` 维护 DMA 和 NVIC 配置，`MX_DMA_Init()` 先于 `MX_USART2_UART_Init()`。
CubeMX 在 `Core/Src/stm32l4xx_it.c` 生成唯一中断入口，分别调用 `HAL_DMA_IRQHandler(&hdma_usart2_tx)`
和 `HAL_UART_IRQHandler(&huart2)`。普通模式 DMA 完成只开启 UART TC 中断，平台在 HAL TX 完成回调中释放信号量。
重新生成后应检查 DMA 句柄关联、两个中断入口唯一、USER CODE 和 Keil 源文件分组。
ISR只收单字节、写1024字节静态环形缓冲并通知debugTask。任务解析命令，
按 20 ms 等待参数轮询异步完成，该参数不是实机往返延迟上限。环形缓冲溢出时清空待处理字节，丢弃当前行到下一个
换行；不会将损坏行的后半段当作新命令执行。

文件二进制会话使用帧魔数、长度和CRC恢复同步，不丢弃到换行。会话期间所有文字命令均暂停解析，避免混入应答。长存储操作返回BUSY，最终应答可按相同请求重放；EXIT清理后保留7.5秒以处理5秒后的重试，再返回文本控制台。重新发送CONSOLE ON后可继续使用上述命令。

存储任务未启动时，文件请求返回最终未就绪错误。此时 EXIT 或空闲退出可结束无在途请求的会话，不会一直按 BUSY 重试；显式 EXIT 的错误应答同样保留7.5秒重放。`FS STATUS` 的 `FLASH` 原因与文件请求的 littlefs 错误分开记录，文件超时可在 `ERRORS` 中显示 TIMEOUT，成功恢复后清除。

HAL 错误按来源处理。纯 TX DMA 错误不清空 RX 缓冲，不标记 RX 故障；TX 失败独立锁存，
即使 RX 重新接收清除了 HAL ErrorCode 或随后到达 TC，也不会误报发送成功。
RX 的奇偶、噪声、帧、溢出和接收超时错误在重新接收前保留，接收端恢复不影响 TX。TX/RX/溢出
故障在完整诊断中通过 DEBUG 来源归入 SYSTEM；成功发送或完整有效命令分别清除相应故障。
2026-10-28起，DBG TX仅保留诊断和累计计数，其发生和恢复不触发屏幕刷新，也不进入屏幕错误页。
RX和溢出仍沿用原上屏规则；ERRORS的类别数和REV包含TX，因此可与屏幕故障快照不同。
累计错误计数不清除。15 ms 是 RTOS 等待参数，调度延迟、串口实际吞吐和完整命令往返时间仍需板测。

## 验证

2026-10-28 TX DMA改造后，四组debug、clock runtime和BW/gray4各36场景显示集成通过；默认Keil完整重建0错误/0警告。UART主机桩覆盖完成/超时/错误、失败后再发送、收发隔离和临界区；具体镜像及日志见[验证记录](测试数据.md)。尚未烧录本次镜像，实机DMA时序、CPU占用和串口连续收发待验收。

2026-10-28文件维护接入后，四组debug主机测试及最终默认/模式7 Keil构建通过；最新精确身份见[验证记录](测试数据.md)。RX已由256B扩为1024B；以下2026-10-27记录为历史范围。

2026-10-27 校时集成后已重跑三套 debug 主机测试并通过，默认黑白整机 Keil 构建为0错误、0警告；显示集成25场景分别通过 BW 和 gray4 配置。日志、资源占用、镜像哈希及源码快照记录见 [校时协议的验证记录](time_protocol.md#2026-10-27-验证记录)。本次文档整理未重跑测试或构建，未烧录，也未进行串口板测。

运行 `./tests/run_debug_host_tests.ps1`，覆盖旧输入 31/32 字节与 WEATHER 127/128 字节边界、UTF-8、CR/LF、整行丢弃、
二进制与文本互斥、异步持久化应答、忙拒绝、全部故障时间输出、主动暂停、
FORMAT 事务未完成时接收天气和切页、失败的退出应答、环形缓冲溢出、HAL 错误保存和 RX 单独恢复。
TIME 用例覆盖完整日期和星期、默认静默且消费结果、提交模式保持、异步成功／失败、重复请求和退出忙拒绝，以及与 FORMAT 双向并行。
原遥测回归入口为 `./tests/run_adxl345_host_tests.ps1`。

上述测试使用主机桩，不证明实机 IRQ、RTOS 调度、栈余量或串口电气性能。
板上需确认持续遥测、进入/退出控制台、保存重启后的格式，以及故障后接收恢复。
