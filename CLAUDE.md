# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Low-power IoT desk clock. MCU: STM32L471RET6 (Cortex-M4, 80MHz, 512KB Flash, 128KB SRAM, LQFP-64). RTOS: FreeRTOS V10.3.1 with CMSIS-RTOS V2 wrapper (already integrated and running). The project is in early setup — the CubeMX HAL + FreeRTOS template compiles in Keil, but the layered application code has not been written.

## Build

**Primary build path: Keil MDK-ARM (ARMCC).** Open `MDK-ARM/DeskClock.uvprojx`, build, and download via ST-Link or equivalent debug probe. This is the only active build path right now.

There is no CMake toolchain yet — root `CMakeLists.txt`, `CMakePresets.json`, and `cmake/gcc-arm-none-eabi.cmake` do not exist. If a CMake path is introduced later, it will be secondary validation alongside Keil.

Key Keil compile outputs land under `MDK-ARM/DeskClock/` (already populated with `.o`/`.d`/`.crf` files).

## Repository Structure

```
Core/              # CubeMX-generated: startup, HAL init, system, interrupts
  Inc/             # main.h, FreeRTOSConfig.h, stm32l4xx_hal_conf.h, stm32l4xx_it.h
                   #   + per-peripheral headers: gpio.h, i2c.h, spi.h, usart.h, tim.h, rtc.h, adc.h
  Src/             # main.c, stm32l4xx_hal_msp.c, stm32l4xx_it.c, system_stm32l4xx.c,
                   #   stm32l4xx_hal_timebase_tim.c
Drivers/           # CMSIS + STM32L4 HAL library (vendor-provided, do not edit)
Middlewares/       # FreeRTOS kernel source (V10.3.1) + CMSIS_RTOS_V2 adaptation layer
MDK-ARM/           # Keil project files, startup, build artifacts
docs/              # Specs, netlist, technical plan (see "Key Docs" below)
DeskClock.ioc      # CubeMX project configuration
```

**Planned directories** (required by spec but not yet created):
- `app/` — business logic, task orchestration, fault recovery
- `device/` — IC driver logic (register protocol, state machine, data conversion; HAL-free)
- `platform/` — HAL-bound adapters for SPI/I2C/UART/GPIO/delay
- `middleware/` — reusable protocol/algorithm/logging modules

## Peripherals — Current State (from generated code and netlist)

All peripherals below are configured in CubeMX and initialized in `main.c`. The HAL handles exist but application-level drivers have not been written.

| Peripheral | Instance | Purpose | Key Pins / Notes |
|:---|:---|:---|:---|
| SPI1 | hspi1 | External Flash (W25Q128JV) | MOSI=PA7, MISO=PA6, SCK=PA5, CS=PA4 |
| SPI2 | hspi2 | E-paper display (GDEY0213B74-FT11, SSD1680Z8) | MOSI=PB15, SCK=PB13, CS=PB2, D/C=PB1, RES=PB0, BUSY=PC5 |
| I2C1 | hi2c1 | Sensor bus: MPL3115A2 (baro) + TMP1075DR (temp) + VEML7700 (ambient light) | SCL=PB6, SDA=PB7; domain switch: SENEN (PC6) via SY6288D20AAC (U15) |
| I2C2 | hi2c2 | ADXL345 (IMU) + MAX17043 (fuel gauge) | SCL=PB10, SDA=PB11 |
| I2C3 | hi2c3 | Touch controller (FT6336U, on display FPC) | SCL=PA8, SDA=PA9 |
| USART1 | huart1 | ESP32-C3 WiFi module | TX=PA9? TX1 on U1.42, RX=PA10? RX1 on U1.43; domain switch: ESPPWREN (PC8) via SY6288D20AAC (U13) |
| USART2 | huart2 | Debug serial (H10 header, 3.3V TTL) | TX=PA2, RX=PA3 |
| ADC1 | hadc1 | Battery voltage sampling | via resistor divider; also NTC thermistor |
| TIM4 | htim4 | Buzzer PWM output | Drives 5.2kHz passive piezo buzzer |
| TIM17 | htim17 | HAL SysTick timebase | Used for `HAL_IncTick()` (FreeRTOS takes SysTick for kernel tick) |
| RTC | hrtc | Wall-clock timekeeping, alarm wake-up | LSE 32.768kHz crystal, powered from backup domain |

### Power Domain GPIOs

These three GPIOs control the SY6288D20AAC load switches to gate peripheral power:

| Pin | Net | Domain | Controls |
|:---|:---|:---|:---|
| PC6 | SENEN | 3V3_SENS_SW | U15 — MPL3115A2, TMP1075DR, VEML7700 |
| PC7 | ELINKEN | 3V3_HMI | U14 — display, touch (FT6336U), front-light |
| PC8 | ESPPWREN | 3V3_WIFI | U13 — ESP32-C3 module |

### Hardware Architecture (from netlist `docs/Netlist_Schematic2_2026-05-21.tel`)

- **Power**: USB Type-C → BQ24074 (U10, charger) → Battery (1000mAh Li-Po) → TPS631000 (U12, buck-boost to 3.3V) → 3× SY6288D20AAC domain switches
- **MCU**: U1 = STM32L471RET6, LQFP-64
- **Sensors**: U2 = ADXL345, U3 = MPL3115A2 (baro), U16 = TMP1075DR (SOIC-8), U18 = MAX17043 (TDFN-8), U37 = VEML7700
- **WiFi**: U20 = ESP32-C3-WROOM-02-N4 (stamp-hole module)
- **Storage**: U29 = W25Q128JV (SOIC-8)
- **Display**: GDEY0213B74-FT11 via FPC connector H8, touch via H9
- **HMI**: BUZZER1 = 5.2kHz passive piezo, SW1-6 = buttons, LED1-2 = status LEDs
- **Debug headers**: H2 (SWD), H10 (USART2 debug serial), H11 (ESP32 debug power bypass)
- **Jumpers**: H1 (BOOT0 select), H3 (NTC/TS select), H4 (debug 3.3V), H5 (ESP32 GPIO9 test), H6 (main 3.3V current measurement), H7 (ESP power path select)

## FreeRTOS Configuration

- **Version**: FreeRTOS V10.3.1 with CMSIS-RTOS V2 wrapper
- **Scheduling**: Preemptive, 1000 Hz tick, max 56 priority levels
- **Memory**: heap_4, 3000 bytes total heap, static + dynamic allocation
- **Timers**: Enabled (priority 2, stack 256 words, queue depth 10)
- **IPC**: Mutexes + recursive mutexes + counting semaphores + event flags + task notifications
- **Features**: Trace facility, task state enumeration, runtime stats hooks available
- **FPU / MPU**: Both disabled
- **Tick**: FreeRTOS uses SysTick; HAL uses TIM17 for `HAL_IncTick()` (see `stm32l4xx_hal_timebase_tim.c`)
- **NVIC**: Max syscall interrupt priority = 5, kernel interrupt priority = 15 (lowest)

## Layer Boundaries (Critical — follow strictly)

```
app ──────────> middleware / device / platform
middleware ───> device (or public interfaces)
device ───────> abstract interfaces (ctx-injected read/write/delay callbacks)
platform ─────> HAL / BSP / Core resources
Core ─────────> system init only, no business logic
```

Rules from `docs/嵌软开发规范.md` and `docs/ic开发规范.md`:

- **device/** must NOT `#include "main.h"`, `"stm32l4xx_hal.h"`, or any HAL header. Must NOT call `HAL_*` functions. All hardware access goes through `ctx` structure containing function pointers (`write_reg`, `read_reg`, `delay_ms`, optional `lock`/`unlock`).
- **platform/** binds the abstract `ctx` interfaces to concrete HAL peripherals (SPI/I2C/UART handles, CS pins, DMA). May depend on HAL headers and `main.h` pin defines.
- **app/** owns business flow and error recovery (retry thresholds, data sanity checks, publish gating after cold-start or reset). Must not touch registers or write low-level bus routines.
- **middleware/** contains reusable algorithms (filters, AHRS, CRC, log formatting). Must not hardcode board-specific pins or peripheral instances.
- **Core/** is generated by CubeMX. Only edit inside `/* USER CODE BEGIN/END */` blocks. Complex logic goes into `app/` / `device/` / `platform/` / `middleware/` files, then call it from USER CODE regions.

## Coding Conventions

- File naming: `lowercase_snake_case`. `.c` and `.h` must share the same base name. No mixed case or inconsistent underscore styles.
- `device/` files: `type_chip` pattern — e.g., `baro_mpl3115a2.c`, `imu_adxl345.c`, `fuel_max17043.c`
- `platform/` files: `stm32_chip` pattern — e.g., `stm32_mpl3115a2.c`, `stm32_adxl345.c`
- `app/` files: business topic names — e.g., `baro.c`, `battery.c`, `display.c`
- Integer types: `stdint.h` fixed-width (`uint8_t`, `int32_t`, etc.). No bare `int`/`long` for hardware-facing data.
- Global variables: `g_` prefix. Static (file-scope) variables: `s_` prefix. Pointer variables: `p_` prefix. Loop counters (`i`, `j`) are the only single-letter variables allowed.
- Local variables: declare at function top (compatibility with old toolchains).
- All register addresses, bit masks, command codes, timeouts, retry counts must be macros or enums — zero magic numbers in logic code.
- Control registers: read-modify-write (read byte → mask off target field → OR in new value → write back). Never blind-write an entire register that contains reserved or independently-configured bits.
- Multi-byte raw data: explicit shift-and-mask assembly. Never `*(uint16_t*)&buf[0]` — this breaks on byte order, alignment, and 24-bit signed values.
- Module-internal helper functions: declare `static` to prevent unintended external linkage.

## Key Docs (read for relevant tasks)

| Document | Content | When to read |
|:---|:---|:---|
| `docs/嵌软开发规范.md` | Project-level: layer architecture, dependency rules, file naming, build constraints, commit checklist | All non-trivial tasks |
| `docs/ic开发规范.md` | IC driver spec: ctx injection pattern, init sequencing, robustness, register modeling, common failures | Any device/sensor task |
| `docs/totalPLAN.md` | Full technical plan: BOM with package constraints, power tree, low-power workflow, dev phases | Architecture or hardware decisions |
| `docs/Netlist_Schematic2_2026-05-21.tel` | PCB netlist: actual pin connections, component footprints | Pin/peripheral mapping questions |
| `.github/copilot-instructions.md` | AI start rules and repository-level guidance | All tasks |
| `.github/instructions/project-structure.instructions.md` | Layer boundaries and build sync rules | Adding/moving source files |
| `.github/instructions/cubemx-generated.instructions.md` | CubeMX file safety rules | Editing Core/ or `.ioc` |

**Rule**: For new device/sensor work, read both `docs/嵌软开发规范.md` and `docs/ic开发规范.md` before writing code. For other non-trivial work, read `docs/嵌软开发规范.md` at minimum. When pin or peripheral mapping is unclear, cross-reference `docs/Netlist_Schematic2_2026-05-21.tel`.

## Before Submitting Changes

1. **Keil build must pass.** Open `MDK-ARM/DeskClock.uvprojx` and verify Project → Build Target compiles without errors.
2. When adding new `.c`/`.h` files, add them to the Keil project groups (right-click group → Add Existing Files) AND to `CMakeLists.txt` if CMake has been set up by that point.
3. In `Core/` files, only modify `USER CODE BEGIN/END` regions. Any block outside those markers will be silently overwritten on next CubeMX regeneration.
4. After CubeMX regeneration, verify: (a) `USER CODE` blocks are intact, (b) FreeRTOS `MX_FREERTOS_Init()` call is still present in `main()`, (c) all user source files still compile in Keil project groups.
5. No `.gitignore` exists yet — `build/`, `MDK-ARM/DeskClock/*.o`, `MDK-ARM/DeskClock/*.d`, `MDK-ARM/DeskClock/*.crf`, IDE user files (`.vscode/` user settings, `.mxproject` user state) should not be committed. Create `.gitignore` before doing large-scale repo snapshots.
