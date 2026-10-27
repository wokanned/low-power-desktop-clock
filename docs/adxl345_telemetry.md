# ADXL345 三轴采集与 JustFloat 遥测

## 版本与范围

状态核对：2026-10-27。三轴采样及 JustFloat 遥测已接入当前工程；下文保留 2026-10-14/15 的构建、主机测试和真实采集记录。历史流没有固件身份字段，不能据此认定当前全模块集成镜像已通过板测。Activity 中断、动作唤醒和 STOP2 尚未实现，整机进度统一见 [totalPLAN.md](totalPLAN.md) 的“当前开发状态（2026-10-27）”。

器件为 ADXL345BCCZ-RL7 三轴加速度计。第一版验证通信、量程、数据符号和安装轴向，暂不实现角速度、姿态融合、FFT、零偏校准或噪声密度分析。温度字段保留，固定为 quiet NaN；温度无效标志始终置位。raw_x/y/z 是 ADXL345 数字输出原始码，不是 MCU PA0 电池 ADC。

源码参考工作区的 `datasheet/主控板资料/加速度计.pdf`（Analog Devices ADXL345 Rev. E），手册印刷页 13、18、24–27。ADXL345 没有软件复位命令和温度数据寄存器。

## 硬件与初始化

- STM32L471RET6，PCLK1=80 MHz；I2C2，PB13=SCL，PB11=SDA。
- SDO/ALT ADDRESS 接地，7-bit 地址 0x53；HAL 使用左移一位后的 0xA6。
- I2C 模式需要 CS 接 VDD_IO，SCL/SDA 需要外部上拉。
- I2C2 Fast-mode 目标为 400 kHz。TIMINGR=0x00702991，模拟滤波开启，数字滤波为 0；实际 SCL 周期和上升沿需示波器/逻辑分析仪验证，该数值不是实测结果。
- `DeskClock.ioc` 与 `Core/Src/i2c.c` 的 USER CODE 扩展块保持相同 TIMINGR。重新生成后须核对这一扩展块、任务入口和 Keil 分组。
- 读取 DEVID（0x00），必须为 0xE5。错误直接退出本次初始化。
- 在 standby 中关闭中断输出，设置 FIFO bypass、Full Resolution、右对齐、±2 g、normal power、ODR=100 Hz，清零 OFSX/Y/Z，最后打开 Measure。
- 控制寄存器读改写并回读检查，保留保留位。软件校准偏移未启用。
- 每次采样先读 INT_SOURCE（0x30）的 DATA_READY，再从 DATAX0（0x32）一次读取六字节。显式按小端补码解码；INT_ENABLE=0 时 DATA_READY/OVERRUN 状态仍可读取。

器件驱动在 `device/imu_adxl345`，HAL 适配在 `platform/stm32_adxl345`，任务和恢复策略在 `app/accel`。当前 accelTask 是 I2C2 唯一运行时用户。平台适配有静态互斥锁；将来接入 MAX17043 时必须让两个适配器共享同一总线仲裁器，不能另建独立锁后并发访问 hi2c2。

## 任务与恢复

accelTask 使用静态控制块和 1024 字节栈，AboveNormal 优先级。通过 RTOS 延时约每 1 ms 检查 DATA_READY，有新数据才读取并生成一帧；传感器 ODR 仍为标称 100 Hz，串口帧不再强制每 10 ms 产生。短暂未就绪时让出 CPU，不发送无效帧、不推进序号、不增加错误计数。debugTask 在 BelowNormal 优先级阻塞发送 UART。八帧静态发送队列占 512 字节，不增加 RTOS 堆占用。平台 I2C 单次 HAL 超时参数 1 ms，最多两次尝试；HAL 的内部 BUSY 检查可能另有更长上限，所以失败时以 LATE 状态和主机时间间隔衡量实际成本，不能把超时参数当作完整事务最坏耗时。

上电先等待 20 ms，进入 Measure 后约 40 ms 内仍读取并清除传感器数据，但通过 STARTUP 标志和 NaN 暂停有效数据发布。初始化失败后，每秒重试，等待期间约每 10 ms 发送一帧带故障状态的固定格式帧。连续三次读取 IO 错误后重新初始化；短暂 NOT_READY 不计入此门槛。距初始化成功或上次成功读取连续 100 ms 无新数据时，增加一次 sample_error_count，按 NOT_READY 故障进入一秒重试等待，期间仍发送故障帧。等待期间保留实际故障原因，不将无新数据故障误报为 CONFIG_ERROR。20/40/100 ms 和三次错误门槛是工程默认策略，需板级冷启动、失联和恢复测试确认。

每次生成数据帧或启动/故障状态帧推进 sequence，队列拒绝帧时仍推进，便于主机检测丢帧。普通的未就绪轮询不生成帧，也不重复发布旧数据。启动预热和真实故障帧的原始值、换算值和滤波值置 NaN。丢队列帧不阻塞采样；延迟后不补发旧周期。时间戳是数据读取完成/故障状态确认时的 MCU 时间，不是传感器硬件采样沿，也不是串口发送时间。100 Hz 是标称传感器速率，实际样本间隔由时间戳分析，允许独立时钟偏差和轮询量化造成的 10/11 ms 等变化。

软件低通采用 `y += alpha*(x-y)`，alpha=0.4665119，即标称 dt=10 ms、fc 约 10 Hz 的指数离散一阶低通。首个有效值直接初始化滤波器；普通未就绪轮询保留滤波状态，仅在新样本到达时更新。通信失败、持续无数据超时或恢复后重置滤波状态，避免跨故障缺口插值。系数保持标称 10 ms 设计，实际截止频率会随采样速率变化。保留芯片 X/Y/Z 顺序，不自动交换或反转安装坐标轴。

## 数据契约

`schema_version=1` 是固件/主机工具共同固定的外部契约，写入采集 JSON；JustFloat 本身不携带版本、固件身份或 CRC。主机 `--schema-version` 只验证预期版本，不能自动证明烧录的固件身份。每帧 16 个 IEEE754 float32，小端，最后追加 `00 00 80 7F`，共 68 字节。USART2 为 PA2=TX、PA3=RX，115200，8N1。VOFA+ 与 Python 顺序使用同一个 COM 口，关闭一个工具后再打开另一个。

| 通道（从 0 起） | 名称 | 单位/含义 |
| --- | --- | --- |
| 0 | timestamp_ms | MCU 毫秒时间，模 2^24 |
| 1 | sequence | 新样本/启动/故障帧生成序号，模 2^24；普通未就绪轮询不递增 |
| 2 | raw_x | 有符号原始码，LSB |
| 3 | raw_y | 有符号原始码，LSB |
| 4 | raw_z | 有符号原始码，LSB |
| 5 | ax_g | raw_x × 0.0039 g/LSB |
| 6 | ay_g | raw_y × 0.0039 g/LSB |
| 7 | az_g | raw_z × 0.0039 g/LSB |
| 8 | filtered_x_g | 软件低通后 X，g |
| 9 | filtered_y_g | 软件低通后 Y，g |
| 10 | filtered_z_g | 软件低通后 Z，g |
| 11 | temperature_c | 固定 quiet NaN，温度无效 |
| 12 | sample_error_count | 初始化失败、采样读取失败、持续无新数据超时的累计次数；普通未就绪不计数 |
| 13 | queue_drop_count | 发送队列满、拒绝新帧的累计次数 |
| 14 | uart_error_count | 调试发送/接收错误累计次数，含部分发送、RX 溢出及损坏或超长控制台命令 |
| 15 | status_flags | 下表位标志 |

采样间隔由主机根据 timestamp/sequence 计算，避免额外通道挤掉三个独立错误计数。计数和标志仍以 float32 发送，但均为整数；三个错误计数到 2^24−1 饱和而不溢出。时间和序号使用 24 位模运算，保持 float32 整数精确：时间约 4 小时 39 分 37 秒回绕，序号约 46.6 小时回绕。报告会区分回绕、回退和有启动证据的复位。超长捕获中断超过半个时间模周期不支持可靠自动判别。

队列/UART 计数由 debugTask 在实际发送前刷新，因此它们是发送时的累计快照；sample_error_count/状态/数据来自同次采样。没有保证传感器内部未读取样本的精确数量，OVERRUN 仅表示发生过覆盖。0.0039 g/LSB 为手册标称系数，不等于已完成实物比例校准。

| 位 | 标志 | 含义 |
| --- | --- | --- |
| 0 | INITIALIZED | 本次读取前初始化及配置回读已成功 |
| 1 | VALID | 本次三轴原始/换算/滤波值有效 |
| 2 | TEMPERATURE_INVALID | 温度不提供，恒为 1 |
| 3 | NOT_READY | 持续无新数据超时，或该原因对应的重试等待；普通未就绪轮询不发送帧 |
| 4 | IO_ERROR | 初始化或读取总线失败 |
| 5 | CONFIG_ERROR | DEVID 错误或寄存器回读失败 |
| 6 | STARTUP | 测量启动预热，数据尚不发布 |
| 7 | OVERRUN | 芯片报告覆盖了未读取的数据 |
| 8 | LATE | 本次任务处理超过一个标称周期 |
| 9 | SATURATED | 某轴达到/超出 ±2 g 的原始边界（−512/+511） |

典型正常状态为 7（INITIALIZED+VALID+TEMPERATURE_INVALID）。USART2 默认只输出固定 JustFloat 帧，无启动横幅和文本日志。显式发送 `CONSOLE ON` 后，debugTask 在完整帧发送结束后暂停遥测并进入文本控制台；`CONSOLE OFF` 成功应答后恢复遥测。控制台期间采样继续，主动暂停不增加 queue_drop_count；序号继续推进，因此不应跨控制台会话连续采集并将该缺口判为通信丢帧。`Debug_GetSuppressedCount()` 单独保留主动暂停计数。`Debug_Log` 仍为空操作。协议和配置命令见 [debug_console.md](debug_console.md)。

## 带宽与观测成本

标称 100 帧/s 时，68 × 100 × 10 = 68000 bit/s，占 115200 bit/s 的约 59%。这超过项目普通低速调试的 10% 建议，属于本次明确指定的实验配置。单帧线上时间约 5.90 ms，UART 超时设为 15 ms。1 ms DATA_READY 轮询增加了 I2C 访问和任务唤醒成本，读取和发送仍分属不同任务。CPU 开销、栈高水位、抖动和电流尚未在目标板测量。100 Hz 连续测量会增加功耗，不代表低功耗产品配置。

## 电脑端

在 `software/DeskClock` 目录运行：

```powershell
py -m pip install -r tools/requirements.txt
py tools/adxl345_capture.py --port COM5 --baud 115200 --duration 60 --csv build/adxl345-run.csv
py tools/adxl345_capture.py --replay build/adxl345-run.bin --no-plot --csv build/adxl345-replay.csv
```

若系统 `py` 不可用，可使用本机已有的 `build/adxl345-python` 独立环境；该环境属于忽略目录，其他电脑需自行创建。依赖尚未安装时先执行第一行。

```powershell
& ./build/adxl345-python/Scripts/python.exe -m pip install -r tools/requirements.txt
& ./build/adxl345-python/Scripts/python.exe tools/adxl345_capture.py --port COM5 --duration 60 --csv build/adxl345-run.csv
```

`--no-plot` 禁用曲线；`--duration 0` 持续直到 Ctrl+C；关闭曲线窗口也会结束采集。COM5 是示例，按实际 USB-UART 端口替换。H10 为 3.3V TTL 串口，电脑侧 USB-UART 的 RX 接 PA2/TX，GND 共地。

输出为同名 `.csv`（数据/主机时间/设备间隔/逐帧质量标记）、`.bin`（完整收到的原始字节，包括坏帧和杂字节）和 `.json`（字段、版本、参数、统计结果）。主机检查完整通道数、整数/状态合法性、原始码与 g 的比例、序号间隙、24 位回绕、时间回退、重复、计数回退、周期偏离、饱和和芯片覆盖。无有效样本或时间/序号/计数回退等返回 fail；丢帧、启动无效、抖动等返回 warn。温度 NaN 是本版正常约定，单独存在时不导致警告。

JustFloat 没有 CRC，字段合法的位损坏仍可能逃过检查；帧同步恢复和质量检查不能作为通信完整性的严格证明。解码失败后从帧尾恢复，保留原始文件用于复查。缺口超过发送计数所能解释的范围标为 unexplained_gap，不凭空指定丢失原因。

## 验证记录

以下记录按日期保留，本轮文档核对未复现采集或测试。构建与主机测试证明相应版本的实现范围，历史真实采集证明当时观察到的三轴响应；均不代表最新天气集成版本的完整验收。

2026-10-14：源码已实现，Keil ARMCC V5.06 update 7 默认模式 0 构建通过，0 errors、0 warnings。ROM=40,496 字节，RAM=23,080 字节；日志/镜像在 `build/adxl345-keil/mode-0`。该结果不证明板上通信、实时性或安装轴向。

主机 C 测试验证 ID/读改写/回读异常、六字节突发读取、字节序和补码边界、未就绪及 IO 返回、JustFloat 编码与失败返回。13 项 Python 测试覆盖分段/逐字节/连续帧、NaN、噪声、半帧、丢字节、错误通道数、序号间隙、计数/时间异常、回绕、复位及 C 编码到 Python 解码。另通过 capture CLI 从 C 输出的真实字节回放生成 CSV/JSON。

2026-10-14 的工程 Python 环境使用 pyserial 和 matplotlib。额外通过 pyserial `loop://` 软件回环把 C 编码帧送入 capture CLI，确认原始 BIN 完全一致并生成两行数据及质量 JSON；用 Agg 后端渲染 1000 个合成样本的三轴曲线，检查三条曲线和坐标可见。该测试验证串口适配与绘图代码，不代表真实 COM 口、交互窗口或板级测试。

```powershell
./tests/run_adxl345_host_tests.ps1
```

如果系统 `py` 不可用，可用 `-Python` 指定 Python 可执行文件绝对路径。主机测试只依赖 Python 标准库与主机 GCC，不需要 pyserial、matplotlib 或硬件。
测试脚本优先使用工程内 `build/adxl345-python/Scripts/python.exe`，不存在时回退为系统 `py`。采集器拒绝覆盖已有 CSV/BIN/JSON，请给每次实验选择新输出名称。

2026-10-15 修改前基线：用户已烧录，COM12/115200 实际采集 30 s，2999 帧、序号连续、设备时间戳间隔均为 10 ms；其中 112 帧为 NOT_READY/NaN（3.73%），间隔 260/270 ms。sample_error_count 增加 112，queue_drop_count 和 uart_error_count 未增加。支持普通未就绪被错误处理为无效帧的判断。原始数据及报告在 `build/com12-20261015-baseline.*`、`build/com12-20261015-baseline-analysis.md`；该采集不代表后续修改的板级验证。

2026-10-15 修正：按 DATA_READY 发布，普通未就绪不计错且保留滤波状态，持续无数据超时与 IO 失败分别恢复。新增任务主机测试直接运行 `app/accel.c`，使用模拟 RTOS 时钟和传感器，覆盖独立 10.4 ms 数据时钟、滤波连续性、tick 回绕、无数据超时恢复、IO 错误恢复和初始化失败重试；Python 测试增加正常 10/11 ms 间隔检查。主机模拟不代表实际总线或 RTOS 调度验证。

该修正已通过任务主机测试、C 驱动/编码测试、14 项 Python 协议测试及采集器回放。证据目录为 `build/adxl345-ready-host-20261015`。Keil ARMCC 默认模式 0 重建通过，0 errors、0 warnings，ROM=40,400 字节、RAM=23,080 字节；日志和镜像位于 `build/adxl345-ready-keil-20261015/mode-0`。镜像 `DeskClock.hex` 的 SHA256 为 `BD978E8C1958C837E145ECB6D9C01C633F91EBF9576F2F89444E46BC36AFB13D`。该次修正在采集前的记录状态为 Build Passed + Host Verified，后续实际采集结果见下文。

2026-10-15 17:07（Asia/Shanghai）平放复测：用户报告设备平放桌面，COM12/115200 采集 30 s，2954 帧全部有效，状态均为 7，序号无缺失、未检测到复位。设备帧间隔为 10 ms（2510 次）和 11 ms（443 次），平均 10.1500 ms，速率 98.5220 Hz。sample_error_count 保持 0，queue_drop_count 和 uart_error_count 均保持 2，本轮未增长。解析器拒绝的两个残片分别是起始 36 字节和结束 1 字节，完整帧之间未发现被拒绝的片段。此前周期性 NOT_READY/NaN 断点在本轮未出现，运行节奏与按 DATA_READY 发布吻合；JustFloat 无固件身份字段，不能独立确认烧录镜像。

平放复测均值为 X=-0.004829 g、Y=-0.000194 g、Z=+0.904492 g；原始码均值分别为 -1.2383、-0.0498、231.9211 LSB。合加速度均值 0.904510 g，比 1 g 低约 9.55%。重力主要落在正 Z 轴，但这不证明比例精度已通过；倾斜本身不能解释合加速度偏低。需要翻面或六面测量区分零点偏移、灵敏度及其他原因，本轮没有实施校准。数据在 `build/com12-20261015-170656-flat.csv/.bin/.json`，统计及曲线在同名前缀的 `-analysis.json/.md/.png`。

2026-10-15 17:19（Asia/Shanghai）倒置复测：用户将整个板子倒置，改用 COM4/115200 采集 30 s。2886 帧全部有效，状态均为 7，序号无缺失、未检测到复位；设备帧间隔 10 ms（1759 次）和 11 ms（1126 次），速率 96.2437 Hz。sample_error_count 保持 0、queue_drop_count 保持 1、uart_error_count 保持 2，本轮均未增长。原始流末尾有 33 字节未完成帧，完整帧之间未发现拒绝片段。历史计数与上一轮不同，不能跨本次串口重连直接作为累计增量；两轮没有可核对的固件身份或同一运行会话证明。

倒置三轴均值为 X=-0.058641 g、Y=-0.026108 g、Z=-1.066492 g，Z 原始码均值 -273.4595 LSB，合加速度均值 1.068432 g。Z 轴由正转负，支持翻面后的符号解析正常。曲线约第 6 s 有一次短暂三轴尖峰，原因未确定；单独统计 10..29.976 s 的 1923 个有效样本，Z 均值 -1.066550 g，与全窗口接近。原始样本全部保留，未删除尖峰。

仅在两轮 Z 输入近似 +1 g/-1 g、配置与比例一致的前提下，两次 Z 均值的中点为 -0.081000 g（-20.7692 LSB），半跨度为 0.985492 g（252.6903 LSB）。这更支持负向零点偏移是正放读数偏低的重要因素，不能将单次 +Z 的约 9.55% 差值直接视为比例误差。倒置 X/Y 均值不为零，姿态和放置接触也可能影响估计；这不是完整六面校准，没有修改固件系数或偏移寄存器。数据在 `build/com4-20261015-171940-inverted.csv/.bin/.json`，两轮比较和曲线在同名前缀的 `-analysis.json/.md/.png`。

2026-10-15 17:35:01（Asia/Shanghai）重复拿起实验：COM4/115200 采集 60 s，用户重复拿起、放下板子，实际动作次数和时刻未标注。收到 5775 帧，三轴全部有效，无三轴 NaN，未检测到复位。设备时间跨度 59987 ms，速率 96.2871 Hz，无序号缺口的相邻样本间隔为 10/11 ms。sample_error_count 保持 0，queue_drop_count 从 8 增至 9，uart_error_count 从 13 增至 14。状态 7 共 5769 帧，状态 519 共 6 帧，没有报告 OVERRUN。本轮数据质量为 warn。

本轮缺失两个序号，发生在相对时间 19.669 s（103986 到 103988）和 19.762 s（103995 到 103997）。原始字节复查显示，第一处缺口恢复同步时丢弃了 52 字节前缀，捕获末尾另有 1 字节残片；不能把全部 53 字节都解释为起止边界。队列/UART 计数首次增长均出现在 19.669 s 的帧中，但它们是发送时刷新快照，不能仅据相邻帧把每一处缺口分别归因。部分 UART 发送失败是候选原因，实物连接或调度方面的原因尚未确定。

在 ±2 g 配置下，有 6 帧 Z 原始码达到 +511（约 +1.9929 g），相对时间为 3.209、3.219、13.407、58.751、58.782、59.343 s。最大已记录合加速度为 2.4053 g。单轴削顶使真实冲击峰值无法由这轮数据确定；合加速度超过 2 g 本身不等于每个轴均超量程。

电脑端离线探索检测使用时间常数 0.5 s 的三轴慢变基线，取原始换算值减去基线后的向量模长作为动作分数。超过 0.15 g 持续至少 30 ms 进入候选动作，低于 0.07 g 持续 500 ms 退出，每段先等待 0.5 s 稳定，序号缺口或无效数据会切断检测段。阈值是本轮诊断参数，没有写入固件，也不等价于 ADXL345 内置 Activity 判定。

本轮得到 12 个候选动作时段，同一参数用于此前平放、倒置静止记录时均得到 0 个候选时段。动作时段可能合并多次拿起，也可能被数据缺口分开；最后一个时段在采集结束时仍活动。用户不知道实际动作次数，因此只保存未标注动作记录，不计算漏检率或检测准确率。这些数据支持三轴采样对拿起、放下动作有明显响应，尚未验证芯片 Activity 中断、MCU 休眠唤醒或各系统上电流程。

原始数据和采集元数据为 `build/com4-20261015-173445-pickup.csv/.bin/.json`。逐帧分数、候选时段和分析报告分别为同名前缀的 `-features.csv`、`-events.csv`、`-motion.json/.md/.png`。本轮离线脚本为 `build/analyze_pickup_capture.py`，复现命令如下；这些实验产物保留在本机被 Git 忽略的 `build/` 目录，本轮未修改或烧录固件。

```powershell
& ./build/adxl345-python/Scripts/python.exe build/analyze_pickup_capture.py build/com4-20261015-173445-pickup.csv --stationary build/com12-20261015-170656-flat.csv --stationary build/com4-20261015-171940-inverted.csv
```

板级仍待验证：DEVID 和配置回读的独立观测、实测 SCL/上升沿、静置六面原始码和 g 的符号及比例、连续遥测的丢帧原因及修正后复测、冷启动预热/失联恢复、栈高水位与 CPU 开销、Activity 中断与低功耗唤醒。
