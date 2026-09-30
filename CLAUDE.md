# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 这个工程在干什么

STM32L471 墨水屏桌面时钟。目标产品要做低占空比联网时钟：温湿度气压采样、NTP 校时、本地闹钟、电池供电跑周级到月级续航。完整方案在根目录 [totalPLAN.md](totalPLAN.md)，那是产品范围的唯一依据，别当草稿改。

当前处于硬件验证阶段。走时、网络、传感器都排在后面。

---

## 仓库布局

```
elinktest/                     # 固件工程（CubeMX + Keil MDK-ARM）
├── Core/                      # CubeMX 生成：时钟、外设、中断、引脚宏
├── app/                       # 业务层，目前只有开机自检
├── device/                    # 器件层，寄存器协议，不碰 HAL
├── platform/                  # 平台层，唯一碰 HAL 的地方
├── Drivers/                   # CMSIS + STM32L4xx HAL
├── MDK-ARM/                   # Keil 工程，唯一能编译的路径
└── elinktest.ioc              # CubeMX 配置（引脚/时钟/外设参数的唯一真源）
```

根目录还有几样东西，找资料前先看这里，别急着联网：

| 路径                                                       | 是什么                                                                    |
| ---------------------------------------------------------- | ------------------------------------------------------------------------- |
| [totalPLAN.md](totalPLAN.md)                                | 产品技术方案：V1 范围、BOM、电源树、任务拆分、六阶段计划                  |
| `Netlist_Schematic2_*.tel`                               | 验证板网表，板级接线的权威来源                                            |
| `*.pdf`                                                  | 各器件规格书：ADXL345、MAX17043、bq24074、TPS631000、ESP32-C3、GDEY029T94-FL03 |
| `output.txt` `datasheet_text.txt`                        | 上面那些 PDF 的提取文本，可直接 grep 寄存器表                             |
| `docs/`                                                  | 项目规范，见下面「最小输入」                                              |
| `.github/`                                               | 上游 CubeMX 模板自带的通用规则，见下面「已知问题」                        |

## 构建与验证

只有一条构建路径：**Keil MDK-ARM**。打开 `elinktest/MDK-ARM/elinktest.uvprojx`。

- 目标 `elinktest`，器件 STM32L471RETx，ARM Compiler 5（V5.06u7），-O3，C99
- 宏只有 `USE_HAL_DRIVER,STM32L471xx`
- 链接用 uVision 自动生成的 scatter，没有外部 `.sct`
- 烧写走 ST-Link（SWD），Flash 算法 `STM32L4xx_512.FLM`，生成 hex
- 上次构建 0 Error 0 Warning，ROM 约 10.58KB

没有测试套件，没有 CMake，没有 Makefile，没有 Lint。验证就是烧进去看屏。

**新增源文件必须手工加进 Keil 工程组**，否则不参与编译。现有分组 `Application/User/App`、`Application/User/Device`、`Application/User/Platform`。

**Keil 的 include 路径里没有 `app/`、`device/`、`platform/` 三个目录**，三层之间全靠 `../` 相对路径互相 include（`main.c` 写 `"../../app/epd_hello_world.h"`）。新加跨层引用照这个写法，或者先往工程里补 include 路径。

## 分层架构

五层单向依赖：

```
app ──→ device / middleware / platform
device ──→ 抽象接口（ctx 注入）
platform ──→ HAL / BSP / Core
Core ──→ 只做系统初始化
```

`middleware/` 目录当前不存在，是规范预留的空位。要用的话先在 Keil 里建组再放源码。

### 各层边界

| 层              | 允许                                             | 禁止                                                                                |
| --------------- | ------------------------------------------------ | ----------------------------------------------------------------------------------- |
| `app/`        | 调 device、platform；持有业务状态                | 直接调 HAL、写寄存器、操作 SPI/I2C 总线                                             |
| `device/`     | `stdint.h`；ctx 注入的函数指针                 | 任何`HAL_*` 调用、include `main.h` 或 `stm32*_hal.h`、include platform 头文件 |
| `platform/`   | HAL 函数、GPIO 寄存器、板级引脚、外设句柄 extern | 业务逻辑、容错策略、告警阈值                                                        |
| `Core/`       | CubeMX 生成的初始化；只改 USER CODE 区           | USER CODE 区外的业务逻辑                                                            |
| `middleware/` | 可复用算法、协议、日志                           | 板级 GPIO、片选、总线实例                                                           |

### device 层的 ctx 注入

device 层看不到任何硬件，只看到函数指针表：

```c
typedef struct {
    int32_t (*spi_write)(void *handle, const uint8_t *data, uint16_t length);
    int32_t (*read_busy)(void *handle, uint8_t *busy_level);
    void (*set_cs)(void *handle, uint8_t level);
    void (*set_dc)(void *handle, uint8_t level);
    void (*set_reset)(void *handle, uint8_t level);
    void (*delay_ms)(void *handle, uint32_t delay_ms);
    uint32_t (*get_tick_ms)(void *handle);
    void *handle;        // 不透明句柄，平台总线上下文
} epd_io_t;
```

platform 层在 `stm32_epd_fill_io()` 里填这张表。屏驱动的调用方要自己提供 framebuffer，缓冲区不归驱动所有。

## 墨水屏要点（GDEY029T94-FL03）

| 参数 | 值 |
| --- | --- |
| 型号 | GDEY029T94-FL03 |
| 尺寸 | 2.9 寸，活动区 29.056×66.896mm |
| 分辨率 | 296×128，像素间距 0.227×0.226mm |
| 驱动 IC | SSD1680Z8 |
| 接口 | SPI 4 线，24Pin 0.5mm FPC |
| 灰阶 | 4 级 |
| 刷新 | 全刷 2s / 快刷 1.5s / 局刷 0.3s |
| 功耗 | 工作 9mW，待机 0.003mW |
| 温度 | 工作 0~50°C，存储 -25~70°C |
| 重量 | 9.15±0.5g |

**前置光**：4 颗 LED 并联，独立 6Pin FPC。Pin 1,2 = 阳极 (A)；Pin 3,4 = NC；Pin 5,6 = 阴极 (K)。Vf 2.75~3.3V（typ 2.8V），If≤60mA。

**显存**：296×128 → 16 bytes/行 × 296 行 = 4736 bytes。Gate 线 296 MUX（0x01 寄存器写 0x27,0x01），Y 窗口范围 0–295。

**无触摸**：这块屏没有集成触摸面板，唤醒由 IMU 负责。

**驱动 IC 和旧屏相同**（SSD1680Z8），24Pin FPC 引脚定义不变，主要改 gate 线数和缓冲区大小。

## 现状

**硬件验证阶段**

- 墨水屏驱动重构中（旧屏方案已废弃，准备切换新屏）
- CubeMX 已配 SPI2、RTC（走时）、GPIO

**还没做**

- app 层读 RTC、时钟界面渲染
- 联网（走 ESP32-C3，UART 自定义协议，不是给 STM32 直接配 WiFi）
- 传感器、蜂鸣器、字库 Flash、电源管理、FreeRTOS
- CMake、`.gitignore`

**配了但没接的**

`hrtc` 在 `main.c` 里初始化了，但全工程没有任何代码用它。

## 硬件事实

### 引脚

| 信号      | 引脚 | 宏名          | 有效电平              |
| --------- | ---- | ------------- | --------------------- |
| SPI2 MOSI | PC3  | —             | AF5                   |
| SPI2 SCK  | PB10 | —             | AF5                   |
| SPI2 MISO | PC2  | —             | AF5，屏不用，但已被占掉 |
| BUSY      | PC5  | `BUSY_Pin`    | **高=忙**，无上下拉   |
| RES       | PB0  | `RES_Pin`     | 低有效                |
| DC        | PB1  | `DC_Pin`      | 低=命令，高=数据      |
| CS        | PB2  | **`CS2_Pin`** | 低有效，GPIO 手动控制 |

两个容易搜不到的点：片选宏叫 `CS2_Pin` 不是 `CS_Pin`；引脚宏统一在 `Core/Inc/main.h`。

### 时钟

HSE 8MHz → PLL（M=1, N=20, R=2）→ SYSCLK 80MHz，AHB/APB1/APB2 全 80MHz。RTC 用 LSE 32.768kHz，异步分频 127、同步 255。

### 外设

- **SPI2**：主机、全双工、8 位、模式 0、MSB 先出、软件 NSS。分频 256 → **312.5 kHz**
- 没有 DMA，没有外设中断，只有 SysTick
- HAL 是裁剪过的：`stm32l4xx_hal_conf.h` 里 **TIM 和 ADC 模块是关的**。要用定时器 PWM 驱蜂鸣器或者 ADC 采电池电压，得同时改三处：`hal_conf.h` 开模块、Keil 工程加对应 HAL `.c`、CubeMX 加外设实例

波特率以 `Core/Src/spi.c:47` 的预分频为准：256 分频 → 312.5 kHz。`elinktest.ioc:205` 缓存的 `CalculateBaudRate=312.5 KBits/s` 和它对得上，这个数可以信。

### 板上总线（重要）

网表里有两条各自独立的传感器 I2C，固件尚未配置：

| 网表标识          | 外设                      | 固件状态   |
| ----------------- | ------------------------- | ---------- |
| `SCL1`/`SDA1` | 气压计、TMP1075、VEML7700 | **未配置** |
| `SCL2`/`SDA2` | ADXL345、MAX17043         | **未配置** |

写传感器驱动前先看网表确认挂在哪条总线上，别按「I2C3 是通用 I2C」直接开写。

## 最小输入

新任务开始前按顺序读：

1. `docs/嵌软开发规范.md` — 项目分层、命名、工作流
2. `docs/ic开发规范.md` — 器件驱动实现规则（寄存器建模、初始化时序、容错恢复、错误码）
3. `.github/copilot-instructions.md` — 通用 AI 工作规则

两份规范要求：事实不明确时先搜仓库，还是不清楚就问用户或标记「待确认」，**不许凭经验补全后当成事实**。

`docs/测试数据.md` 是空的。

## 编码硬规则

- **device 层不许出现 HAL。** 所有硬件访问走 ctx 注入的函数指针
- **不许有魔数。** 寄存器地址、位掩码、命令字、超时、重试次数全部写成宏或枚举
- **每个硬件等待都要有超时。** 不许对 BUSY 或外设标志做无限 `while`
- **控制寄存器用读-改-写。** 掩码加移位，不用位域
- **CubeMX 生成文件只改 USER CODE 区。** 重新生成后检查 include 区、变量区、函数声明区、调用点是否还在
- **只用在本文件内的函数写 `static`**
- **文件命名小写 snake_case。** `app/` 用业务主题名，`device/` 用 `类型_器件名`，`platform/` 用 `stm32_器件名`，`.c` 与 `.h` 同名配对
- **CTX 的 `handle` 必须被真正用上。** 不许留了字段却在实现里硬编码全局句柄
- **总线附加位（SPI/I2C 读写位、多字节自增位）在 platform 层处理**，不许漏到 device 或 app
- **多字节数据用移位和掩码拼**，不许把缓冲区强转成 `uint16_t*`

## 已知坑

**BUSY 引脚无上下拉**，完全靠面板驱动。虚焊或悬空时固件会一直读到 0（空闲），等待立刻通过且不报错，表现为屏刷不出来但代码全绿。排查这类问题先量 PC5 的实际电平。

**SPI NSS 脉冲隐患**：CubeMX 默认生成的是 `SPI_NSS_PULSE_ENABLE`。软件手动翻转 CS 的用法下开着 NSS 脉冲是个隐患，以后改 `.ioc` 的分频或 NSS 设置会影响屏，代码里看不出来。

**待确认**：气压计型号在项目内自相矛盾。`totalPLAN.md` 写 MPL3115A2，网表里 U3 的封装写 `MS5611-01BA03`。两者寄存器和数据格式完全不同，动手写驱动前先核对实物。

**版本管理现状差**：仓库里只有 6 个文件被 git 跟踪，`app/`、`Core/`、`Drivers/`、`MDK-ARM/`、`.github/`、`docs/` 下大部分内容都是未跟踪状态，也没有 `.gitignore`，根目录散落着一堆临时产物。不具备「拉取即构建」的条件。

**`.github/` 下的规则整体当噪音处理**：那套文件是上游 CubeMX 模板原样带过来的，把 CMake 当主构建路径（仓库里根本没有 CMakeLists），把 Keil 写成「以后可能引入的可选项」（实际上 Keil 是唯一能编译的路径）。而且它的 `applyTo` glob 按仓库根布局写（`app/**`、`*.ioc`、`MDK-ARM/**`），真实路径都在 `elinktest/` 下，很可能从来没生效过。里面唯一值得留的是「不许凭经验猜」那几条工作流规则。

## 中文行文风格

写中文说明时：短句、常用词。不用「链路」「回路」「闭环」「耦合」这类词，除非技术上是必要的。讲边界时直接说允许什么、不允许什么，别堆否定。写给人的规范要像团队规则，不像提示词。
