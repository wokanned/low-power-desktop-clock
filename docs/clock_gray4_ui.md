# 四灰阶时钟界面

日期：2026-10-18。当前状态：Implemented / Build Passed / Host Verified。
目标板画面、时序、IRQ 和任务栈余量尚未验证。

## 页面与资源

生产页面为 296x128 横屏四灰阶，无秒、无触摸。左侧时间和日期星期，右侧
TEMP/BAT、WIFI/内存四格；分隔线 x=160、228。温度计、电池、Wi-Fi 和空心圆环
由离线位图资源提供，左下角不再显示日历图标。主时间 Bahnschrift weight650、
size76、width75，按原生像素栅格化，保留四灰阶边缘与较强的实黑笔画。
数字及缺失占位使用34px固定格，冒号使用12px格；裁除字形空白后居中，左右
至少留一像素。总宽固定148px，范围x8..155；分钟两位分别从x88、x122开始。
正常页日期使用 size18/width95，标签 size12/width100，数值 size24/width90，
Wi-Fi 时间 size17/width100；这些字体使用 weight700、原生像素栅格化和实黑笔画，
每个字形后至少留一像素间距。四个信息图标也使用原生实黑轮廓。仍发送完整 2bpp
帧，主时间、月亮和分隔线保留灰阶。仅提交所需字符和图标，运行时不解析字体。

日期星期仍在同一行，从x8开始；最长日期宽131px。内存圆环为14px，位置
(234,78)，标签从x250开始，`IFLASH`宽38px。其余四格布局保持不变。

`tools/generate_clock_assets.py --check` 验证生成资源与源字体一致。字模含
10180 字节有效位图；需使用脚本指定的字体版本和 Pillow 环境重现。
测试使用真实 C 渲染器输出图像，而非浏览器示意图。

### 实机照片对应的可读性修复

2026-10-18 的照片中，主时间较完整，日期与四格小字笔画断裂。源码检查发现旧小字
经过四倍超采样缩小、窄体压缩后，部分字形位图宽度还大于字符推进距离。
相同照片数据的旧 C 帧中，日期50.29%、标签53.37%至56.62%的非白像素为灰色；
新正常页小字及信息图标只使用白/黑，避免细笔画依赖尚未校准的物理灰度表现。
这修正字形和字距问题，照片本身不能证明控制器的真实四级灰度有故障。

`build/clock-aligned-host/`保存实际C渲染的照片同数据、最长日期和三种内存页面，
以及1:1和整数放大的旧新对照。black-only软件消融图只移除旧帧灰色像素，
用于暴露旧字的实黑骨架，不是实机灰度或屏幕效果模拟。

主时间的旧比例字宽使`11:11`仅94px，`04:44`达144px，分钟起点随小时变化。
固定格修复这一漂移，并通过原生字号栅格化和微增字重优化分钟笔画。

原生帧为 128x296、9472 字节，2bpp MSB first，0=白、3=黑。逻辑坐标映射：

```text
native_x = 127 - y
native_y = 295 - x
byte = native_y * 32 + native_x / 4
shift = 6 - 2 * (native_x % 4)
```

## 模型、任务与刷新

`clock_view_model_t` 是纯值快照。`UNAVAILABLE`、`INITIALIZING`、`ERROR` 字段
显示 N/A，只有 READY 数值可展示。当前 TEMP/BAT/WIFI/W25Q使用量没有提供者，
不创建虚假的正常值或故障。ADXL345 不支持温度，不把其 temperature-invalid 位报为 TEMP。

defaultTask 每秒读取完整 RTC 日历，每十秒缓存内存统计；星期由公历日期计算。
RTC 读取先 Time 后 Date，失败也尝试释放 shadow 锁。备份标记只代表初始化，
未校准的默认日历仍正常显示。RTC 读取/范围失败进入 RTC 类，顶栏为 --:-- / N/A，
不会令显示传输重试或挂起。

`clock_fault_report()` 由生产者在任务上下文报告，快照在短临界区复制。
来源保存 active/first 原因、首次 tick、首次完整日历和有效标记；类别保存整次
类别故障的首次时间。相同原因重复报告不修改修订号和时间。类别部分恢复不改
首次时间，完全清除后再次发生建立新记录。可用 RTC anchor 不超过两秒，按
单调 tick 补足秒数；失效时不伪造发生时间。已恢复来源的最近一次记录保留在 RAM，
不跨复位持久化。

固定七类 RTC/TEMP/BAT/WIFI/SENSOR/STORAGE/SYSTEM；SENSOR 合并 IMU/BARO/LIGHT，
STORAGE 合并 W25Q/CFG，SYSTEM 合并 TASK/DBG。七行字号14、行距14，顶栏同时
放置小时间、日期和星期。单一来源显示简短原因，多个来源列出来源；完整掩码和
各自发生时间由 ERRORS 输出。错误时间及错误页顶栏使用24h，正常页可切12h并显示AM/PM。
EPD 故障不进入屏幕类别；停机异常没有新增崩溃恢复或持久化报告。

首刷等待时钟服务和配置读取的终态。正常每分钟全刷；故障修订、格式或 RTC
可用性变化触发额外全刷。RTC 不可用且其他状态不变时不重复刷相同错误页。
刷新期间合并变化，成功后只检查最新模型，不补播历史帧。

每帧从硬复位和现有原厂 GRAY4 初始化开始，再发送 24/26 两平面和 C7 激活，
BUSY 完成后提交，之后 sleep。未增加未经证实的灰阶 RAM 地址命令。
BUSY 结束前不得推进显示版本或内存轮换。失败重试使用最新快照和相同内存阶段，连续三次、间隔一秒；
成功重置预算，耗尽只挂起显示任务。sleep 失败只留 EPD 状态，不重复提交。

内存格以 SRAM -> IFLASH -> W25Q 轮换，首次 SRAM；只在正常页成功提交后推进。
错误页不推进，轮换不额外触发刷新。Wi-Fi小时数0..99显示 Nh ago，100及以上99h+。

## 内存与配置

SRAM 显示“已分配/保留”，分母131072字节：

```text
linked = sum(Image$$RW_IRAMn$$ZI$$Limit - Image$$RW_IRAMn$$Base)
used = linked - xPortGetFreeHeapSize()
```

静态栈/帧缓冲、启动栈和C heap保留空间均已包含；RTOS堆数组在linked中，
因此动态TCB/栈不能再次相加，也不减stack high-water。两块非连续SRAM逐region求和。
IFLASH 分母524288字节，统计 `Load$$LR$$LR_IROM1$$Limit - Base`，包含压缩后的
RW初值与实际加载区开销。链接器符号只取地址，不解引用。百分比四舍五入。
W25Q没有文件系统/容量分配统计，保持N/A。

24h默认、12/24格式配置由storageTask独占的静态请求队列保存。双独立4KiB扇区
0x00FFE000、0x00FFF000，避开既有0x0000F000启动测试区。提交算法、损坏恢复及
保留的破坏性自检见 `w25q128_storage.md`。串口显式控制台协议见 `debug_console.md`。

## 构建与验证

当前 Keil ARMCC5 默认模式0：0 errors / 0 warnings。
模式0 Code=46116、RO=13852、RW=300、ZI=35876字节；Program Size ROM合计60268，
RAM合计36176。当前加载区压缩后实际Flash跨度59992字节，SRAM1跨度36176，
SRAM2跨度0，均已对照新map符号。UI使用实际加载跨度，不混用历史map数字。
镜像：`build/clock-aligned-keil/mode-0/DeskClock.hex`；SHA256：
`3FBE699C9D63DC294A40E53B23DEA875E8C4DDCC3E4B4BE94C1AA33B76D4D3DB`。

```powershell
.\tests\run_clock_view_host_tests.ps1 -OutputDirectory .\build\clock-aligned-host
.\tests\run_clock_runtime_host.ps1 -OutputDirectory .\build\clock-runtime-host
.\tests\run_clock_display_host.ps1 -OutputDirectory .\build\clock-aligned-display
.\tests\run_clock_config_host.ps1 -OutputDirectory .\build\clock-config-host
.\tests\run_debug_host_tests.ps1 -OutputDirectory .\build\clock-debug-host
.\tests\run_gui_host_tests.ps1 -OutputDirectory .\build\clock-gui-host
.\tests\run_vendor_host_tests.ps1 -OutputDirectory .\build\clock-vendor-host
.\tests\run_w25q128_host.ps1 -OutputDirectory .\build\clock-w25q-host
.\tests\run_adxl345_host_tests.ps1 -OutputDirectory .\build\clock-accel-host
.\tests\build_vendor_modes.ps1 -Modes @(0) -OutputDirectory .\build\clock-aligned-keil
```

主机覆盖全天1440时分、36525日期、两种格式和三种内存、七类同时错误、帧边界、
9472字节编码与方向、首发时间和聚合恢复、tick回绕、RTC与内存缓存、十一种实际显示
调度场景、三轮灰阶事务故障注入、4128掉电切断、存储服务和控制台边界。
这些证据不模拟或证明控制器两平面自动地址回绕、物理灰度、残影及耗时。

本次可读性修复通过95565项绘制检查和12个实际C帧，额外断言正常页小字/图标
使用实黑像素、字间留白、主时间固定格、分钟跨小时位置稳定以及日期无日历图案；
四格状态与上版逐像素相同，11种显示调度场景重新通过。
双平面发送与远端参考一致，本次不修改波形、驱动、引脚或刷新策略。

## 引脚配置与板测

2026-10-19用户确认实物仍使用原板接口PC3 MOSI，串口和其他功能正常。
此前CubeMX再生成已将输出改为PB15，造成屏幕接线与固件不符；现将`.ioc`和生成的
SPI2配置统一为PC3 MOSI、PB10 SCK、分频32，并保留PC5的BUSY标签。
USART2 priority5 IRQ由CubeMX在Core生成唯一入口，platform保留HAL接收和错误回调。
后续再生成须保留上述映射，并核对RTC保时、FreeRTOS入口和Keil用户源文件分组。

板上待验收：确认上述实际引脚与持续供电；不对称四角/四级灰块连续刷新及平面配准；
分钟与故障刷新、三次重试与串口继续运行；控制台切换和格式重启保持；
各任务stack high-water、刷新时长与长期残影。尚未烧录，不声称HW Verified或Accepted。
