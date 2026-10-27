# 时钟界面与黑白/四灰阶输出

状态更新：2026-10-27。工程总进度以 [totalPLAN.md 的“当前开发状态（2026-10-27）”](totalPLAN.md#当前开发状态2026-10-27) 为统一入口。本文件说明时钟页面、输出契约及分版本验证记录。

生产默认 `EPD_VENDOR_DEMO_MODE=0`、`EPD_CLOCK_USE_GRAY4=0`，使用黑白分钟全刷；四灰阶保留为可选输出。当前源码已增加串口天气切页，10 月 27 日有主机测试通过记录，当前完整集成的 Keil 链接和整机板测仍待完成。黑白正式页面尚未完成上板验证；四灰阶专用页已有实屏失败记录，不能视为四灰阶验收通过。

## 页面与资源

生产页面为 296x128 横屏，无秒、无触摸。左侧时间和日期星期，右侧
TEMP/BAT、WIFI/内存四格；分隔线 x=160、228。温度计、电池、Wi-Fi 和空心圆环
由离线位图资源提供，左下角不再显示日历图标。主时间 Bahnschrift weight650、
size76、width75，按原生像素栅格化；离线资源保留四级覆盖率，生产黑白输出按
覆盖率码值>=2画黑、<2留白，与模式7左侧BW数字的规则一致。
数字及缺失占位使用34px固定格，冒号使用12px格；裁除字形空白后居中，左右
至少留一像素。总宽固定148px，范围x8..155；分钟两位分别从x88、x122开始。
正常页日期使用 size18/width95，标签 size12/width100，数值 size24/width90，
Wi-Fi 时间 size17/width100；这些字体使用 weight700、原生像素栅格化和实黑笔画，
每个字形后至少留一像素间距。四个信息图标也使用原生实黑轮廓。黑白默认输出完整1bpp
帧，月亮同样按半覆盖阈值画黑，所有非白结构线画实黑以保留细线。
`EPD_CLOCK_USE_GRAY4=1`恢复2bpp输出，主时间、月亮和分隔线保留原灰阶。
仅提交所需字符和图标，运行时不解析字体；不进行放大或抖动。

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

四灰阶原生帧为128x296、9472字节，2bpp MSB first，0=白、3=黑。逻辑坐标映射：

```text
native_x = 127 - y
native_y = 295 - x
byte = native_y * 32 + native_x / 4
shift = 6 - 2 * (native_x % 4)
```

黑白原生帧为128x296、4736字节，1bpp MSB first，1=白、0=黑。沿用上述坐标，
字节地址为`native_y * 16 + native_x / 8`，位号为`7 - native_x % 8`。
两种格式共用布局代码，直接绘制到目标缓冲，无中间灰阶帧或运行时缩放。
现有9472字节静态缓冲继续供可选四灰阶页面及互斥演示使用；默认黑白时钟和天气页只绘制和发送前4736字节。

## 模型、任务与刷新

`clock_view_model_t` 是纯值快照。`UNAVAILABLE`、`ERROR` 字段显示 N/A，只有
READY 数值可展示。`INITIALIZING` 通常显示 N/A，W25Q 使用量在此状态显示
`SCAN`，使用既有标签字模，从(236,102)绘制，宽28px。当前 TEMP/BAT/WIFI 没有提供者，
不创建虚假的正常值或故障。ADXL345 不支持温度，不把其 temperature-invalid 位报为 TEMP。

defaultTask 每秒读取完整 RTC 日历，每十秒缓存内存统计；星期由公历日期计算。
RTC 读取先 Time 后 Date，失败也尝试释放 shadow 锁。备份标记只代表初始化，
备份域首次初始化或失电后为 `2000-10-18 00:00:00`，未校准的默认日历仍正常显示。
普通复位在备份域供电保持时保留日历；当前没有 NTP 或控制台校时入口。天气消息的
时间仅作标签，不设置 RTC。RTC 读取/范围失败进入 RTC 类，顶栏为 --:-- / N/A，
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

首刷等待时钟服务首次轮询和存储配置终态。当前默认启动 Flash 自检开启，初始化成功后
先擦写 `0x0000F000..0x0000FFFF` 专用 4KiB 区，再发布配置 ready；首刷包含这段等待，
但不等待后续全片容量扫描完成。正常每分钟全刷；故障修订、格式或 RTC
可用性变化触发额外全刷。RTC 不可用且其他状态不变时不重复刷相同错误页。
刷新期间合并变化，成功后只检查最新模型，不补播历史帧。

默认每帧从硬复位和现有原厂BW_296初始化开始，4736字节图像写入24并同步26旧图RAM，
F7激活后等待BUSY完成、提交和sleep。`EPD_CLOCK_USE_GRAY4=1`仍使用原厂GRAY4初始化、
9472字节转换为24/26两平面和C7激活，再等待BUSY、提交和sleep。
未增加未经证实的波形、温度或RAM地址配置。
BUSY 结束前不得推进显示版本或内存轮换。失败重试使用最新快照和相同内存阶段，连续三次、间隔一秒；
成功重置预算，耗尽只挂起显示任务。sleep 失败只留 EPD 状态，不重复提交。

内存格以 SRAM -> IFLASH -> W25Q 轮换，首次 SRAM；只在正常页成功提交后推进。
错误页不推进，轮换不额外触发刷新。Wi-Fi小时数0..99显示 Nh ago，100及以上99h+。

当前默认选中时钟页。天气数据由 USART2 的 `WEATHER` 命令注入，`PAGE WEATHER` /
`PAGE CLOCK` 只修改 RAM 中的页面选择；复位清空天气并回到时钟页。天气页按分钟、
可见字段或格式变化全刷，隐藏时接收数据只更新缓存；故障页覆盖所选页，故障清除后
恢复最终选择。天气帧不推进时钟内存格轮换。协议和三小时过期规则见
[weather_protocol.md](weather_protocol.md)。这些入口尚未接入 ESP32 自动联网。

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
W25Q 使用 `storage_get_usage_snapshot()` 的 RAM 快照，不在显示或时钟服务中读 Flash。
storageTask 启动后分段扫描全片16MiB，每个4KiB扇区只要有一字节非0xFF就将整个
扇区计为已用，包含配置、自检与残留内容。每轮结束后等待1小时再次全扫。
首次扫描及重扫均发布 INITIALIZING，轮到 W25Q 格时显示 SCAN；扫描成功后发布
READY，容量按非空扇区数计算，百分比向上取整。空片0%，非空最低1%，最多100%。
未启动为 UNAVAILABLE；扫描失败为 ERROR，丢弃本轮结果、显示N/A并报告既有
STORAGE故障，1小时后重试。存在故障类别时仍使用既有错误页。
时钟服务只接受总容量为 `W25Q128_TOTAL_SIZE` 且已用容量不超过总量的 READY
快照，其余 READY 数据按 ERROR 处理。容量状态变化不额外触发刷新、不阻塞首刷，
沿用分钟刷新和 SRAM -> IFLASH -> W25Q 轮换；扫描状态可能在下一次 W25Q 轮次才上屏。

24h默认、12/24格式配置由storageTask独占的静态请求队列保存。双独立4KiB扇区
0x00FFE000、0x00FFF000，避开既有0x0000F000启动测试区。提交算法、损坏恢复及
保留的破坏性自检见 `w25q128_storage.md`。串口显式控制台协议见 `debug_console.md`。

## 构建与验证

### 当前集成状态（2026-10-27）

当前天气、控制台和显示调度有主机测试通过记录，详见 [weather_protocol.md](weather_protocol.md)。
本次文档整理未重新执行测试。现有主目录镜像为 10 月 20 日，较新的默认黑白镜像为
10 月 21 日，均早于天气集成；当前完整工作区的 Keil 链接、镜像包含、任务栈余量与
正式黑白/天气页面板测仍待完成。以下数据是对应日期的历史记录，不代表当前占用或验收。

### 四灰阶时钟与可读性修复（2026-10-18，历史记录）

该阶段 Keil ARMCC5 默认模式0：0 errors / 0 warnings。
模式0 Code=46116、RO=13852、RW=300、ZI=35876字节；Program Size ROM合计60268，
RAM合计36176。当时加载区压缩后实际Flash跨度59992字节，SRAM1跨度36176，
SRAM2跨度0，均已对照该阶段map符号。UI运行时使用实际加载跨度，不硬编码这些历史数字。
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

该阶段可读性修复通过95565项绘制检查和12个实际C帧，额外断言正常页小字/图标
使用实黑像素、字间留白、主时间固定格、分钟跨小时位置稳定以及日期无日历图案；
四格状态与上版逐像素相同，11种显示调度场景重新通过。
该阶段双平面发送与远端参考一致，未修改波形、驱动、引脚或刷新策略。

### W25Q 使用量验证（2026-10-20）

`tests/run_clock_runtime_host.ps1 -OutputDirectory build/w25q-usage-runtime-host`
已通过。覆盖容量快照的临界区嵌套、无需轮询立即更新、状态传播、异常总容量、
已用量越界，以及0字节、1字节、1/40/41/2048/2049/4095/4096扇区的向上取整。
同时确认扫描不阻塞首刷、使用量变化不增加刷新、SRAM/IFLASH十秒缓存维持原行为。

`tests/run_clock_view_host_tests.ps1 -OutputDirectory build/w25q-usage-view-host`
已通过146118项实际C绘制检查。新增逐像素验证 SCAN 的位置与28px字宽、0%/1%/100%
和N/A，以及首次扫描、重扫、失败之间无残留、W25Q格外像素不变，其他内存格的
INITIALIZING仍显示N/A。输出17个实际C帧，既有12帧仍通过预览检查；新增5帧的
PNG位于同一输出目录，`clock-gray-w25q-states.png`为状态合图，已目视检查。
上述主机证据不代表板上 SPI 扫描耗时、任务栈余量或显示效果验收。

### 四灰阶专用诊断页（2026-10-20）

新增`EPD_VENDOR_DEMO_MODE=7`，当时Keil默认选择此模式；0仍为正式时钟，1–6仍为
原厂演示。切换须在工程级Define中修改同一个宏，重新编译烧录。头文件缺省为0。
测试页显示四级色块、同一原生字模0689的黑白/灰阶对照、四色线条和圆环、不对称角标。
复用既有2bpp坐标映射、字模、帧缓冲、驱动和灰阶初始化，不修改正式时钟布局。

测试成功后屏幕休眠并保持画面，displayTask挂起，不清屏也不返回时钟。其他任务继续。
任何事务错误均按三次上限完整重试，包含休眠失败；这不会推进时钟提交或内存轮换。

`tests/run_gray4_diagnostic_host.ps1`通过29647项像素检查和16个真实显示/绘制/驱动场景，
覆盖成功保持、单次入口，以及绑定、平台准备、初始化、刷新、BUSY错误/超时、休眠的
瞬时失败恢复和持续三次耗尽。模拟SPI轨迹验证两平面均为4736字节且内容与真实C绘制一致，
没有黑白清屏或时钟读取。原模式0的11个显示场景和146118项时钟绘制检查通过；
vendor回归1039447项检查及80种合法/非法宏组合通过。
输出位于`build/gray4-diagnostic-host/`，预览来自实际C帧，不能证明实屏灰度或休眠电流。

2026-10-20该阶段Keil ARMCC5整工程重建通过，两个模式均为0错误、0警告。历史构建结果：

| 模式 | Code | RO | RW | ZI | ROM合计 | RAM合计 |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| 7诊断页 | 46212 | 13860 | 316 | 35508 | 60388 | 35824 |
| 0正式时钟 | 47884 | 13860 | 316 | 36004 | 62060 | 36320 |

独立镜像和日志位于`build/gray4-diagnostic-keil/mode-7/`和`mode-0/`。模式7镜像
`DeskClock.hex`的SHA256为`C77FC2348E101A6EF5E439496CBCB59480C747FD0217D256E6ECAD99DA42B9D8`；
模式0为`0997CC5D0444ABD7AF09FF7801362B96A4739F5126A448ADF44E4B03C97E30FB`。
链接map确认模式7选择专用renderer、移除正常时钟renderer；模式0选择正常时钟renderer。
首次沙箱内构建无法访问用户目录的Keil设备包；允许沙箱外执行后按原设备包完成重建。
未更改设备包、驱动波形、引脚或烧录设备。

板测时检查0白、1浅灰、2深灰、3黑依次可分，四角方向正确，BW与GRAY4数字位置一致；
GRAY4圆弧边缘应呈灰色过渡。源码成功提示为`display: gray4 diagnostic asleep; page retained`，
但当前 `Debug_Log` 为空实现，不会在串口输出该行。通过状态查询及实屏保持确认结果，
观察超过一分钟确认未被时钟覆盖，同时确认串口仍运行；四灰阶实屏历史结果见下一节，
休眠电流仍待测量。

### 黑白正式页面（2026-10-21）

2026-10-20诊断照片中0/2均为白、1/3均为黑，码值2的第三组线条不可见。
右侧GRAY4边缘出现毛刺，左侧BW更干净。此结果推翻此前口头的“四灰阶通过”判断，
只能确认当前实屏未产生四级灰度，不能据此认定屏幕完全不支持。原厂示例已注明旧版
不支持4G；用户提供FPC标识`029BN-T94FL03,A6G`，批次能力仍未确认。
按用户要求暂时搁置灰阶排查，不增加BUSY观测或未经验证的驱动参数。

当前生产选择为`EPD_VENDOR_DEMO_MODE=0,EPD_CLOCK_USE_GRAY4=0`，两者均已进入Keil定义。
新`epd_clock_view_draw_bw()`复用原布局和离线资源，按模式7左侧规则二值化，细结构线
单独保留为实黑。驱动复用原BW_296初始化、全刷和休眠，不更改SPI、引脚、寄存器或波形。
分钟调度、三次重试、刷新提交、错误页和W25Q的SCAN状态保持既有行为。
宏`EPD_CLOCK_USE_GRAY4=1`恢复原灰阶时钟；`EPD_VENDOR_DEMO_MODE=7`仍为保留的诊断页。

离线比较原生单色hinting与weight600/650/700，没有发现一致更好的轮廓，因此保留已获
实屏反馈的字模和固定格。预览在`build/clock-bw-analysis/`，它们只用于比较像素轮廓，
不模拟墨水扩散、对比度或残影。纯黑白可避免错误灰色带来的毛刺，不能消除125dpi
像素栅格固有的阶梯。

主机验证：真实C渲染器的黑白和灰阶共521704项检查，覆盖所有时刻、日期、布局边界、
全页阈值/结构线及缓冲哨兵；17张黑白预览均只有0/255。调度、提交、重试和休眠的11个
独立场景分别覆盖两个宏值。保留诊断页29647项像素检查及16个集成场景通过；
原厂驱动1039447项检查和80种模式组合通过。资源生成器`--check`通过，资源文件未修改。
主机产物与实际C帧预览位于`build/clock-bw-host/`。当前`Debug_Log`为空实现，历史文字
日志只能作为代码位置说明，不能当作已收到的串口或硬件验证证据。

2026-10-21该阶段Keil ARMCC5完整重建通过，三个选择均为0错误、0警告，尚不包含后续天气集成：

| 模式/输出 | Code | RO | RW | ZI | ROM合计 | RAM合计 |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 / 黑白默认 | 48140 | 13860 | 316 | 36004 | 62316 | 36320 |
| 7 / 原灰阶诊断 | 46268 | 13860 | 316 | 35508 | 60444 | 35824 |
| 0 / 灰阶宏1 | 47960 | 13860 | 316 | 36004 | 62136 | 36320 |

默认黑白镜像为`build/clock-bw-keil/mode-0/DeskClock.hex`，SHA256
`A0DC8AB3C5F6827DA33ECAC01BE6B6BABB084E3C710EB877E5CB2795B4EA378E`。
保留诊断镜像为`build/clock-bw-keil/mode-7/DeskClock.hex`，SHA256
`B2BF5FB4D9170313A6F6B7856315F9E931F16E37CE311F24843A7737DFCE76FF`。
灰阶宏1镜像为`build/clock-bw-keil/gray4/mode-0/DeskClock.hex`，SHA256
`8323BC21B2B8DC343106F54CD0A2B12B61BAAC5C487F4E23B306CC8F7E4B7383`。
黑白map确认正式调用`epd_clock_view_draw_bw`和`epd_gdey029t94_full_refresh`，
灰阶正式renderer、诊断renderer和灰阶refresh被链接器移除。设备层及平台层没有修改。
最终构建使用本机已安装设备包；沙箱内无法读取设备包时未改变包或工程设备选择。

## 引脚配置与板测

2026-10-19用户确认实物仍使用原板接口PC3 MOSI，串口和其他功能正常。
此前CubeMX再生成已将输出改为PB15，造成屏幕接线与固件不符；现将`.ioc`和生成的
SPI2配置统一为PC3 MOSI、PB10 SCK、分频32，并保留PC5的BUSY标签。
USART2 priority5 IRQ由CubeMX在Core生成唯一入口，platform保留HAL接收和错误回调。
后续再生成须保留上述映射，并核对RTC保时、FreeRTOS入口和Keil用户源文件分组。

板上待验收：确认上述实际引脚与持续供电；黑白页面方向、笔画、分隔线与残影；
分钟与故障刷新、三次重试与串口继续运行；控制台切换和格式重启保持；
各任务stack high-water、刷新时长与长期残影。截至2026-10-27，当前正式黑白及天气集成版本没有可核对的下载与整机验收记录，不声称HW Verified或Accepted。
