# AGENTS.md

本文件为 Codex（Codex.ai/code）在本仓库中处理代码提供指导。

## 工程概况

目标为低功耗 IoT 桌面时钟。MCU 为 STM32L471RET6（Cortex-M4，80MHz，512KB Flash，128KB SRAM，LQFP-64），使用 FreeRTOS V10.3.1 和 CMSIS-RTOS V2。2026-10-27 当前处于软件集成原型阶段，已接入 RTC/黑白时钟显示、ADXL345 遥测、W25Q 配置存储和 USART2 注入天气；自动联网、环境/电量采集、按键闹钟及整机低功耗尚未实现。

**工程进度统一维护在 [docs/totalPLAN.md](docs/totalPLAN.md) 的“当前开发状态（2026-10-27）”。** 最新天气整合有主机测试记录，完整 Keil 构建及整机验收待完成。模块文档保留具体历史证据；源码接入、历史构建或局部实采均不能代替当前完整镜像的板测。

## 构建

**固件构建方式：Keil MDK-ARM（ARMCC）。** 打开 `MDK-ARM/DeskClock.uvprojx`，编译后通过 ST-Link 或同类调试探针下载。这是目前唯一的整机固件构建入口。`tests/` 下主机 GCC 测试是逻辑/模拟验证，不能代替固件链接或板测。

目前尚未配置 CMake 工具链，根目录下不存在 `CMakeLists.txt`、`CMakePresets.json` 和 `cmake/gcc-arm-none-eabi.cmake`。如果后续引入 CMake，应将其作为 Keil 之外的辅助验证方式。

Keil 默认输出到 `MDK-ARM/DeskClock/`，历史独立验证产物位于 `build/`。目录中已有镜像不代表覆盖当前源码；其日期、模式和验证范围见统一进度入口。

## 仓库结构

```
Core/              # CubeMX 生成：启动、HAL 初始化、系统和中断
  Inc/             # main.h、FreeRTOSConfig.h、stm32l4xx_hal_conf.h、stm32l4xx_it.h
                   #   以及各外设头文件：gpio.h、i2c.h、spi.h、usart.h、tim.h、rtc.h、adc.h
  Src/             # main.c、stm32l4xx_hal_msp.c、stm32l4xx_it.c、system_stm32l4xx.c、
                   #   stm32l4xx_hal_timebase_tim.c
Drivers/           # CMSIS + STM32L4 HAL 库（厂商提供，禁止修改）
Middlewares/       # FreeRTOS 内核源码（V10.3.1）+ CMSIS_RTOS_V2 适配层
app/               # 时钟/故障、显示/天气、加速度、调试/VOFA和存储业务
device/            # 无 HAL 依赖的 GDEY029T94、ADXL345、W25Q128 驱动
platform/          # STM32 显示、I2C加速度、Flash、RTC和调试串口适配
gui/               # 复用的厂商 Paint 绘图代码（全局画布）
fonts/             # 复用的拉丁字符和中文字库
resources/         # 厂商显示资源及离线时钟/天气字形
MDK-ARM/           # Keil 工程文件、启动文件和构建产物
docs/              # 规范与技术方案（见下方“关键文档”）
DeskClock.ioc      # CubeMX 工程配置
```

`middleware/` 仍是规划中的可复用协议、算法和日志模块目录。

## 外设当前状态

下表列出 CubeMX 配置和 `main.c` 初始化资源，不表示每个器件都有业务实现。GDEY029T94、ADXL345、W25Q128 已有 device/platform/app 分层及启动入口；环境传感器、电量、ESP32 和提醒业务仍待接入，详细状态见 `docs/totalPLAN.md`。

| 外设   | 实例   | 用途                                                                 | 关键引脚 / 说明                                                                              |
| :----- | :----- | :------------------------------------------------------------------- | :------------------------------------------------------------------------------------------- |
| SPI1   | hspi1  | 外部 Flash（W25Q128JV）                                              | MOSI=PA7，MISO=PA6，SCK=PA5，CS=PA4                                                          |
| SPI2   | hspi2  | GDEY029T94-FL03，SSD1680Z8，296×128                                 | MOSI=PC3 AF5，SCK=PB10 AF5，CS=PB2，D/C=PB1，RES=PB0，BUSY=PC5；2.5MHz（80MHz / 32）         |
| I2C1   | hi2c1  | 传感器总线：MPL3115A2（气压）+ TMP1075DR（温度）+ VEML7700（环境光） | SCL=PB6，SDA=PB7；电源域开关：SENEN（PC6），通过 SY6288D20AAC（U15）控制                     |
| I2C2   | hi2c2  | ADXL345（IMU）+ MAX17043（电量计）                                   | SCL=PB13，SDA=PB11                                                                           |
| USART1 | huart1 | ESP32-C3 WiFi 模块                                                   | TX=PA9（U1.42），RX=PA10（U1.43）；电源域开关：ESPPWREN（PC8），通过 SY6288D20AAC（U13）控制 |
| USART2 | huart2 | 调试串口（H10 排针，3.3V TTL）                                       | TX=PA2，RX=PA3                                                                               |
| ADC1   | hadc1  | 预留电池电压采样，尚未接入业务                                       | PA0 接电池分压；电池 NTC 接到 BQ24074 TS，不接此 ADC                                         |
| TIM4   | htim4  | 预留蜂鸣器 PWM，尚未启动响铃业务                                     | 5.2kHz 无源压电蜂鸣器，当前仅完成外设配置                                                     |
| TIM17  | htim17 | HAL SysTick 时间基准                                                 | 用于`HAL_IncTick()`（FreeRTOS 使用 SysTick 作为内核节拍）                                  |
| RTC    | hrtc   | 日历计时已接入；闹钟提醒与低功耗唤醒业务待实现                       | LSE 32.768kHz；备份域供电时使用 RTC_BKP_DR0 标记保留日历；冷初始化为 2000-10-18 00:00:00    |

### 电源域 GPIO

以下两个 GPIO 控制 SY6288D20AAC 负载开关，用于控制 WiFi 和传感器供电。墨水屏在系统供电期间持续上电，显示负载开关已移除。

当前 SENEN/ESPPWREN 初始化后保持低电平，尚无按事件切换电源域的流程。低电平开启仅为项目约定，实际极性和输出电压仍待确认，不能把 GPIO 配置当作已经断电的证据。

| 引脚 | 网络名   | 电源域             | 控制对象                            |
| :--- | :------- | :----------------- | :---------------------------------- |
| PC6  | SENEN    | 3V3_SENS_SW        | U15：MPL3115A2、TMP1075DR、VEML7700 |
| PC8  | ESPPWREN | 3V3_WIFI           | U13：ESP32-C3 模块                  |

### 硬件架构

显示移植使用 Git 中 `origin/EinKTest@8583f09` 保留的主板和显示转接板网表，并与 `Core/Inc/main.h` 和 `Core/Src/` 中的外设初始化进行核对。连接器映射记录在 `docs/epd_gdey029t94_port.md` 中。当前显示供电已改为持续上电，历史网表中的显示负载开关连接不再适用。已删除的网表未恢复到工作区。在目标板验证前，必须确认实物板版本和接线。

- **电源**：USB Type-C 和 1000mAh 锂聚合物电池 → BQ24074（U10，充电/电源路径管理）→ BATPOWER → TPS631000（U12，升降压至 3.3V）；墨水屏持续供电，WiFi 和传感器使用 2 个 SY6288D20AAC 电源域开关
- **MCU**：U1 = STM32L471RET6，LQFP-64
- **传感器**：U2 = ADXL345，U3 = MPL3115A2（气压），U16 = TMP1075DR（SOIC-8），U18 = MAX17043（TDFN-8），U37 = VEML7700
- **WiFi**：U20 = ESP32-C3-WROOM-02-N4（邮票孔模块）
- **存储**：U29 = W25Q128JV（SOIC-8）
- **显示**：GDEY029T94-FL03，SSD1680Z8，296×128，无触摸。主板 H8.6（MOSI）必须连接转接板 H1.7（SDI），H8.7（SCK）必须连接 H1.6（SCLK）；这两个引脚不是直通连接。
- **人机交互**：BUZZER1 = 5.2kHz 无源压电蜂鸣器，SW1-6 = 按键，LED1-2 = 状态指示灯
- **调试接口**：H2（SWD），H10（USART2 调试串口），H11（ESP32 调试供电旁路）
- **跳线**：H1（BOOT0 选择），H3（NTC/TS 选择），H4（调试 3.3V），H5（ESP32 GPIO9 测试），H6（主 3.3V 电流测量），H7（ESP 电源路径选择）

## FreeRTOS 配置

- **版本**：FreeRTOS V10.3.1，使用 CMSIS-RTOS V2 封装层
- **调度**：抢占式，1000 Hz 节拍，最多 56 个优先级
- **内存**：heap_4，总堆大小为 3000 字节，同时支持静态和动态分配。显示/存储任务各有 2048 字节静态栈，加速度任务有 1024 字节静态栈；实际栈裕量待板测。
- **定时器**：已启用（优先级 2，栈大小 256 个字，队列深度 10）
- **任务间通信**：互斥锁、递归互斥锁、计数信号量、事件标志和任务通知
- **功能**：可使用跟踪功能、任务状态枚举和运行时间统计钩子
- **FPU / MPU**：`configENABLE_FPU=1`，`configENABLE_MPU=0`，以 `Core/Inc/FreeRTOSConfig.h` 为准
- **节拍**：FreeRTOS 使用 SysTick；HAL 使用 TIM17 调用 `HAL_IncTick()`（见 `stm32l4xx_hal_timebase_tim.c`）
- **NVIC**：允许调用系统 API 的最高中断优先级值为 5，内核中断优先级为 15（最低）
- **当前任务**：defaultTask、debugTask、accelTask、displayTask、storageTask；Tickless、Stop 2 和运动唤醒尚未接入。

## 显示模块集成

- 复用远端 `EinKTest` 的设备驱动、GUI、字库和资源，不得随意重构协议或资源。`device/` 保持不依赖 HAL；`platform/stm32_gdey029t94` 负责绑定 SPI2、控制引脚和延时。
- `app/display` 独占显示控制器、帧缓冲区和全局 `Paint` 画布。在 RTOS 初始化阶段通过 `display_start()` 启动。禁止从其他任务或 ISR 调用绘制、演示或刷新函数。
- `app/weather` 只在 RAM 中缓存经校验的完整天气消息并跟踪单调 tick 过期状态；`app/display` 合并选中页面、天气与时钟为一致快照，天气渲染由 `app/epd_weather_view` 完成。切页或天气页可见内容变化触发全刷；天气页隐藏时新数据只更新缓存，不能因此刷新屏幕。串口协议见 `docs/weather_protocol.md`。
- `app/epd_bringup` 的生产时钟默认黑白分钟全刷：每帧复位、黑白初始化、完整图像写入并同步旧图 RAM、F7 激活、BUSY 完成后提交和休眠。字模边缘按半覆盖阈值二值化，结构线保持实黑。`EPD_CLOCK_USE_GRAY4=1` 可选择原四灰阶路径；默认0。错误集合或持久化格式变化申请额外全刷；快刷/局刷仅保留在演示与诊断路径。
- 正式时钟使用 `EPD_VENDOR_DEMO_MODE=0`，头文件及 Keil 当前默认均为0，并统一设置 `EPD_CLOCK_USE_GRAY4=0`。模式7仍保留专用四灰阶诊断页，刷新和休眠成功后保留画面，仅挂起显示任务，不进入时钟；模式1–6为原厂演示，完成后进入所选格式的时钟。两个宏独立，必须在整个工程统一设置，编号见显示移植文档。当前实屏四灰阶验收未通过，不将其解释为面板确定不支持。
- 显示启动/恢复最多尝试三次，失败后间隔一秒再尝试。尝试耗尽后记录保留的状态，并仅挂起显示任务。调度器启动后，平台延时通过 RTOS 让出执行权。
- 墨水屏持续供电，初始化、错误恢复和休眠策略均不控制显示供电开关。前光独立控制方式仍待确认。
- `.ioc` 与生成的 SPI2 初始化必须保持一致，预分频为 32（2.5MHz）。普通 MCU 复位应保留 RTC 备份标记和日历。
- 显示信号连接以对应板版本的原始网表为准，`.ioc`、Core 和文档按网表同步，不能反过来用固件配置推断接线。当前参考提交 `8583f09` 的主板网表为 `MOSI2 ; H8.6 U1.11`，对应 PC3 AF5；BUSY 为 `H8.2 U1.25`，对应 PC5，并保留 `BUSY` 标签。USART2 中断入口由 CubeMX 生成，平台仅保留 HAL 接收和错误回调。完整映射见 `docs/epd_gdey029t94_port.md`。
- `defaultTask` 每秒运行时钟服务、每十秒缓存内存统计；只有显示任务绘制。不存在的数据提供者显示 N/A。静态故障表按七类聚合，EPD 故障只保留诊断，不上屏。
- USART2 默认 JustFloat；WEATHER 和 PAGE 命令在两种串口模式下均可接收，JustFloat 模式静默处理；显式 `CONSOLE ON` 后暂停遥测并启用控制台应答，支持 FORMAT、ERRORS、WEATHER 和 PAGE。weather 数据只缓存在 RAM；storageTask 独占配置事务，预留 W25Q 最后两个独立扇区，不可与启动测试区混用。
- `Debug_Log()` 当前为空实现，不能期待文字启动日志。EPD 自身状态通过控制台 `ERRORS` 或调试器查看。
- `STORAGE_BOOT_TEST_ENABLE` 默认 1，每次启动擦写 `0x0000F000..0x0000FFFF` 专用扇区；配置在 `0x00FFE000`、`0x00FFF000` 两个扇区。首屏等待时钟轮询及自检后的配置 ready。该联调策略尚未收为产品启动策略。

## 分层边界（必须严格遵守）

```
app ----------> middleware / device / platform
middleware ---> device（或公共接口）
device -------> 抽象接口（通过 ctx 注入读/写/延时回调）
platform -----> HAL / BSP / Core 资源
Core ---------> 仅负责系统初始化，不放业务逻辑
```

以下规则来自 `docs/嵌软开发规范.md` 和 `docs/ic开发规范.md`：

- **device/** 禁止 `#include "main.h"`、`"stm32l4xx_hal.h"` 或任何 HAL 头文件。禁止调用 `HAL_*` 函数。所有硬件访问必须通过包含函数指针的 `ctx` 结构体完成（`write_reg`、`read_reg`、`delay_ms`，以及可选的 `lock`/`unlock`）。
- **platform/** 将抽象 `ctx` 接口绑定到具体 HAL 外设（SPI/I2C/UART 句柄、CS 引脚、DMA）。允许依赖 HAL 头文件和 `main.h` 中的引脚定义。
- **app/** 负责业务流程和错误恢复（重试阈值、数据合理性检查、冷启动或复位后的数据发布控制）。禁止直接访问寄存器或编写底层总线操作。
- **middleware/** 放置可复用算法（滤波、AHRS、CRC、日志格式化）。禁止硬编码特定板卡的引脚或外设实例。
- **Core/** 由 CubeMX 生成。仅允许在 `/* USER CODE BEGIN/END */` 区域内修改。复杂逻辑应放在 `app/`、`device/`、`platform/` 或 `middleware/` 文件中，再从 USER CODE 区域调用。

## 编码规范

- 新文件使用小写下划线命名（`lowercase_snake_case`）。对应的 `.c` 和 `.h` 文件必须同名。保留导入的厂商名称，例如 `gui/GUI_Paint` 及字库/资源表名称，不在移植时重命名。
- `device/` 文件使用 `type_chip` 格式，例如 `baro_mpl3115a2.c`、`imu_adxl345.c`、`fuel_max17043.c`
- `platform/` 文件使用 `stm32_chip` 格式，例如 `stm32_mpl3115a2.c`、`stm32_adxl345.c`
- `app/` 文件按业务主题命名，例如 `baro.c`、`battery.c`、`display.c`
- 整数类型使用 `stdint.h` 中的定宽类型（`uint8_t`、`int32_t` 等）。硬件相关数据禁止使用裸 `int`/`long`。
- 全局变量使用 `g_` 前缀。文件作用域的静态变量使用 `s_` 前缀。指针变量使用 `p_` 前缀。仅循环计数器（`i`、`j`）允许使用单字母变量名。
- 局部变量在函数开头声明，以兼容旧工具链。
- 所有寄存器地址、位掩码、命令码、超时和重试次数必须定义为宏或枚举；逻辑代码中禁止魔法数字。
- 控制寄存器使用读改写：读取字节 → 清除目标字段 → 按位或写入新值 → 写回。禁止直接整字节覆盖包含保留位或独立配置位的寄存器。
- 多字节原始数据必须通过显式移位和掩码拼装。禁止使用 `*(uint16_t*)&buf[0]`，因为这种写法会在字节序、对齐及 24 位有符号数据处理上出错。
- 模块内部辅助函数声明为 `static`，防止意外产生外部链接。

## 关键文档（按任务阅读）

| 文档                                                       | 内容                                                                | 阅读时机                           |
| :--------------------------------------------------------- | :------------------------------------------------------------------ | :--------------------------------- |
| `docs/嵌软开发规范.md`                                   | 工程级规范：分层架构、依赖规则、文件命名、构建约束、提交检查清单    | 所有非简单任务                     |
| `docs/ic开发规范.md`                                     | IC 驱动规范：ctx 注入模式、初始化顺序、健壮性、寄存器建模、常见故障 | 所有设备/传感器任务                |
| `docs/嵌入式遥测与实验调试工作流.md`                     | 遥测契约、实验准备、观测、日志和调试工作流                          | 遥测、串口观测、实验或硬件调试任务 |
| `docs/totalPLAN.md`                                      | 统一工程进度、默认上电行为、下一阶段，以及产品目标/BOM/电源树      | 开始任务、判断进度或架构决策       |
| `docs/epd_gdey029t94_port.md`                            | 显示源码来源、GPIO/连接器映射、刷新/任务归属、验证状态              | 显示移植或硬件调试                 |
| `docs/clock_gray4_ui.md`                               | 黑白/四灰阶时钟界面、快照与故障、内存统计及历史验证               | 时钟界面和数据接入                 |
| `docs/weather_protocol.md`                              | 串口天气输入、RAM 缓存、过期和页面切换                            | 天气数据和天气页                   |
| `docs/debug_console.md`                                 | JustFloat/控制台切换、时制和故障查询                              | 调试控制台                         |
| `docs/w25q128_storage.md`                                | Flash 自检区、配置持久化和物理占用扫描                            | 存储与配置                         |
| `docs/adxl345_telemetry.md`                              | 三轴采样、遥测及历史实板采集                                      | 加速度数据和串口观测               |
| `docs/adxl345_stop2_guide.md`                            | 尚未接入构建的低功耗与动作唤醒设计                                | 低功耗实现规划                     |
| `.github/copilot-instructions.md`                        | AI 启动规则和仓库级指导                                             | 所有任务                           |
| `.github/instructions/project-structure.instructions.md` | 分层边界和构建同步规则                                              | 新增/移动源文件                    |
| `.github/instructions/cubemx-generated.instructions.md`  | CubeMX 文件修改安全规则                                             | 修改 Core/ 或`.ioc`              |

**规则**：新增设备/传感器相关工作，必须在写代码前阅读 `docs/嵌软开发规范.md` 和 `docs/ic开发规范.md`。其他非简单任务至少阅读 `docs/嵌软开发规范.md`。引脚或外设映射不明确时，先检查 `Core/` 中的引脚定义和初始化，再核对文档记录的网表版本及实物板。显示参考映射和未确认的硬件事项见 `docs/epd_gdey029t94_port.md`。

## 提交变更前

1. **Keil 构建必须通过。** 打开 `MDK-ARM/DeskClock.uvprojx`，确认通过 Project → Build Target 编译无错误。
2. 新增 `.c`/`.h` 文件时，必须加入 Keil 工程分组（右键分组 → Add Existing Files）；如果届时已配置 CMake，还必须加入 `CMakeLists.txt`。
3. 修改 `Core/` 文件时，仅允许改动 `USER CODE BEGIN/END` 区域。这些标记之外的内容会在下次 CubeMX 重新生成时被直接覆盖。
4. CubeMX 重新生成后，检查：（a）`USER CODE` 区域完整；（b）`main()` 中仍保留 FreeRTOS 的 `MX_FREERTOS_Init()` 调用；（c）所有用户源文件仍能在 Keil 工程分组中编译。
5. 现有 `.gitignore` 已排除 `build/`、Keil 输出/DebugConfig 目录、目标文件/列表文件/固件镜像、Keil 用户设置、`.vscode/`、`.mxproject` 和本地 Claude 设置。禁止将生成产物和个人 IDE 状态提交到仓库。
