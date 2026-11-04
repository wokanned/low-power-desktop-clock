# 多天天气预报与断电保存

**当前串口操作前提（2026-11-04）**：默认 AUTO 不持续遥测，睡眠时不能保证串口接收。先在上电30秒或晃动后的10秒窗口发送 `POWER HOLD`，收到 `OK POWER HOLD` 后再执行本文示例；结束调试可发送 `POWER AUTO`。仅 `CONSOLE ON` 不会保持清醒。详见[低功耗说明](low_power.md)。

文档核对日期：2026-11-04。当前源码的已知缺陷、与用户已确认但尚未实现的行为差异，见[代码与需求差距](current_code_gaps.md)。本文不将待修复项写成已完成。

本接口保存当前城市的连续1～7天预报。每条记录带完整日期，STM32按北京时间RTC日期选择当天记录。每日预报没有来源更新时间，也不套用实况的三小时过期规则。ESP32负责联网、天气API转换和取得当前网络时间；当前固件接收入口为USART2，未实现ESP32联网固件。

## 先校时，再发天气

USART2使用H10、115200、8N1、UTF-8、实际CR/LF/CRLF行结束。每行等待成功应答再发下一行，示例中的TIME须替换为发送时的真实北京时间：

```text
CONSOLE ON
TIME 2026-10-29T09:31:00
FORECAST BEGIN 42,深圳,3
FORECAST DAY 42,2026-10-29,2,180,270
FORECAST DAY 42,2026-10-30,3,190,260
FORECAST DAY 42,2026-10-31,4,170,240
FORECAST COMMIT 42
FORECAST STATUS
CONSOLE OFF
```

依次应答`OK CONSOLE ON`、`OK TIME`、`OK FORECAST BEGIN 42`、三条`OK FORECAST DAY 42`、`OK FORECAST COMMIT 42`。STATUS输出保存信息和事务状态，最后应答`OK CONSOLE OFF`。更新过程中旧预报继续可用；最终COMMIT成功才表示已完成Flash持久保存，屏幕刷新仍可能在进行。多行连续粘贴不能代替逐行等待。

STM32直接信任收到的北京时间，允许TIME向前或向后调整；仅校验格式、日历合法性和RTC写入/同步/读回是否成功，不与旧RTC比较误差或限制偏移。每个新的预报批次都必须在当前控制台会话内先完成一次TIME。RTC失败则不获得接收许可，不提交新天气。TIME成功后即使天气保存失败，RTC也不回滚。详见[校时协议](time_protocol.md)。

BEGIN消耗一次校时许可；取消或暂存超时后，新批次需重新TIME。收集或保存期间TIME返回BUSY，须先完成或取消天气事务。进入/退出控制台会清除未使用的许可。FORECAST仅在控制台模式处理；原WEATHER、TIME的默认静默兼容行为保留。

## 命令与更新规则

| 命令 | 含义 |
|---|---|
| `FORECAST BEGIN 编号,城市,天数` | 暂存一个新批次；城市为深圳或湛江，天数1～7 |
| `FORECAST DAY 编号,YYYY-MM-DD,天气码,最低温,最高温` | 加入一个完整日期的每日记录 |
| `FORECAST COMMIT 编号` | 校验整批，并异步保存；最终完成才应答成功 |
| `FORECAST CANCEL 编号` | 取消尚未开始保存的暂存批次 |
| `FORECAST STATUS` | 查看当前保存批次、日期范围、在途状态和最近结果 |

批次编号为非零uint32，发送端每份不同内容使用新编号。仅保存最新一份批次，旧编号不作为永久历史去重索引。命令大小写敏感，FORECAST每行最多127个UTF-8字节，不计行结束符。空字段、额外字段、空白、引号和溢出数值均不接受。

每日记录可乱序发送，提交时按日期升序处理；必须正好收齐声明的天数，各日期连续且不重复。相同日期相同内容重发只确认一次；同日期不同内容拒绝。日期范围为2000～2099年，必须是真实日历，不能跨越2099年末。新提交的首日不得早于提交时的RTC当天，可从未来日期开始。跨午夜后若原首日已经过去，需取消并重建正确日期的批次。

天气码0～13沿用[实况协议](weather_protocol.md)；温度为0.1°C，范围−999～999，最低不得高于最高。例如180表示18.0°C。预报不包含未来的“当前温度”、湿度或风速。

同一个BEGIN在收集中可重发，不隐式清掉已收记录。收集期间连续60秒没有有效BEGIN/DAY会超时释放。保存已开始后不能取消，也不能强制释放后台请求。文件维护与天气保存互斥；收集/保存期间仅允许FS STATUS，其他FS操作返回BUSY，完成或取消后再维护。

## 应答与重试

| 应答 | 含义与处理 |
|---|---|
| `OK FORECAST BEGIN/DAY/CANCEL 编号` | 暂存、该条接收或取消完成，不代表天气已落盘 |
| `OK FORECAST COMMIT 编号` | 整批已持久保存并发布，或确认相同的已保存批次 |
| `ERR FORECAST TIME` | 新批次缺少成功TIME，或RTC不可用 |
| `ERR FORECAST DATE` | 新批次含早于RTC当天的日期 |
| `ERR FORECAST ARGUMENT` | 命令或字段不合法 |
| `ERR FORECAST SEQUENCE` | 批次不匹配、缺记录或日期不连续 |
| `ERR FORECAST CONFLICT` | 相同编号或相同日期的内容冲突 |
| `ERR FORECAST STORAGE 编号 错误码` | 存储拒绝或失败；旧RAM预报保持，RTC已校正则保持新时间 |
| `ERR FORECAST TIMEOUT 编号` | 收集超时，重新TIME后开始新批次 |
| `ERR BUSY` | 正在保存或其他事务占用，稍后查询/重试 |

STATUS的`FORECAST SAVED`行显示实际已发布批次的编号、城市、天数和首末日期，0表示无保存预报；`FORECAST STATE`行显示IDLE/COLLECTING/SAVING、已收条数、可用TIME许可、恢复状态、存储结果和最近事务结果。

最终应答丢失时，先查询STATUS，再重发`FORECAST COMMIT 原编号`。当前保存批次的重复COMMIT不重新校时、不写Flash、不切页，也不重新记录同步成功。整批重发相同已保存编号时仍会比较全部内容，完全相同才成功，即使已经跨日也允许确认历史结果。不同内容必须使用新编号并重新TIME。

保存过程中报错或复位可能发生在原子rename边界，Flash恢复后可能是完整旧批次，也可能是完整新批次。没有收到成功应答不等于没有写入，应重连后核对STATUS；不得把不确定结果当成一定失败并换编号覆盖。TIME重试时重新采集当前时间，不重发旧时间字符串。

## Flash、恢复与显示

文件为littlefs内的`/forecast.bin`，占用25～79字节有效载荷，实际分配还包含文件系统元数据。格式为magic `DCFC`、版本1、城市、天数、保留字节、批次编号、每日日期/天气码/高低温和IEEE CRC32，全部整数显式小端编码。使用已有临时文件、sync、读回CRC和rename原子替换，不直接写C结构体。

`storageTask`独占Flash。设计为挂载成功后自动加载并校验文件；当前默认AUTO启动的恢复推进可能延迟一小时，见[代码与需求差距](current_code_gaps.md)，不能把尚未恢复误判为文件不存在。恢复不校时、不切天气页、不重置WIFI同步记录。缺文件为无预报；坏版本、长度、CRC或字段会拒绝该文件并报告预报恢复错误，不把内容错误当作littlefs损坏。未格式化、满盘和IO故障均不自动格式化；首次使用按[文件系统说明](filesystem.md)明确执行格式化。

通用FS可列目录、查询、下载该文件，禁止覆盖上传、删除或改名。`FS FORMAT CONFIRM`仍会删除整个littlefs分区，成功后清空RAM预报和校时许可；原WEATHER实况不清除。配置区及OTA维护预留区不受天气功能改动。

以下数据选择只适用于无WDATA已发布批次的旧天气兼容路径；当前WDATA优先及未来回退见[代码与需求差距](current_code_gaps.md)。没有预报时保留原实况行为；有预报时，仅同城、来源日期为RTC当天且接收未满三小时的实况优先，否则显示当天预报。预报只显示天气及高低温，当前温度、湿度、风向和风速为`--`，页脚为预报适用日期。当天无记录显示“无当天预报”，RTC不可用显示等待校时。午夜或TIME校正后重新选择对应日期，保存的过去记录不在后台逐条擦除。

## 分层与验证

`app/forecast`负责有界模型、日期规则、文件编码和RAM快照；`app/forecast_protocol`负责分行事务及TIME许可，通过send回调应答，未绑定HAL或UART实例。`app/debug`负责当前控制台和命令路由；实际USART2收发、中断、DMA仍在`platform/stm32_debug_uart`。将来迁移USART1时调整平台绑定、CubeMX/IRQ/DMA配置和路由，新预报模型、协议及存储不需要改写。当前不同时启动两个串口客户端。

验证入口为`tests/run_forecast_host_tests.ps1`、`tests/run_forecast_protocol_host.ps1`、`tests/run_sync_protocol_host.ps1`和`tests/run_storage_files_integration_host.ps1`，加原实况、文件维护及显示回归。Keil完整构建与本轮验证记录见[测试数据](测试数据.md)。主机模拟和构建不能代替真实Flash掉电、串口时序、跨日实屏和各任务栈高水位板测。
