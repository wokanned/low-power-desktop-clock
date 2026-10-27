# ADXL345 动作中断与 STM32L471 STOP2 配置指南

设计日期：2026-10-17；状态核对：2026-10-27。适用工程：STM32L471RET6、FreeRTOS V10.3.1 / CMSIS-RTOS V2、Keil ARMCC。

本文件是配置教程与代码设计，动作唤醒及 STOP2 状态仍为 Planned，本文示例尚未加入当前构建。当前工程已有 LPTIM1 的 LSE、DIV1 和优先级 5 中断配置，但没有自定义 Tickless 或 STOP2 入口，ADXL345 仍关闭中断输出并轮询采样。部分外设配置已存在，不代表低功耗功能已经完成。整机进度统一见 [totalPLAN.md](totalPLAN.md) 的“当前开发状态（2026-10-27）”。

## 1. 选择 STOP2

目标流程是 ADXL345 持续测量并自行判断 Activity，STM32 进入 STOP2。传感器中断脚产生上升沿后，EXTI 唤醒 STM32；恢复运行时钟后，由任务读取中断原因，再按需要恢复显示、WiFi 和其他传感器。

| 模式 | 主控状态 | 对本项目的用途 |
| --- | --- | --- |
| Sleep | CPU 停止，普通外设时钟可运行 | 早期验证空闲休眠，功耗通常高于 STOP2 |
| STOP2 | CPU 和大部分时钟停止，SRAM 与寄存器保留 | 推荐的正常待机模式，可由 GPIO EXTI 和 RTC 唤醒 |
| Standby / Shutdown | 唤醒走复位启动流程，主运行上下文不保留 | 另行设计关机状态，不直接用于恢复现有 RTOS 任务 |

ADXL345 是加速度计。它能响应起动、停止、振动和姿态变化造成的加速度变化，不保证检出任意缓慢拿起或持续匀速运动。芯片 Activity 与此前 Python 的三轴基线残差算法不同，需要单独测触发率与误触发。

MCU STOP2 与 ADXL345 自身的 Sleep 是两个独立设置。第一步保持传感器原有 Full Resolution、±2 g、100 Hz、Measure=1；先完成主控 STOP2。之后再评估传感器 LOW_POWER 或 AUTO_SLEEP，避免同时改变检测延迟与噪声。传感器 standby（Measure=0）不能继续检测动作。

## 2. 先核对中断接线

仓库历史网表 `origin/EinKTest@8583f09:Netlist_Schematic2_2026-05-21.tel` 给出：

| 传感器信号 | 历史连接 | 当前工程情况 |
| --- | --- | --- |
| ADXL345 INT1，U2.8 | ACCINT1 -> U1.41 -> PA8 | `.ioc` 错标为 ACCINT2，生成宏为 ACCINT2A8 |
| ADXL345 INT2，U2.9 | ACCINT2 -> U1.40 -> PC9 | `.ioc` 标为 ACCINT2 |
| 气压计中断 | U3.6 -> PB3，U3.5 -> PB4 | 当前宏 INT1 / INT2，不是 ADXL345 的中断脚 |

以下使用 **ADXL345 INT1 -> PA8**。这项依据来自历史设计；实际焊接板须核对 U2.8 与 PA8 的连接及电平，再按此配置。若实物仅连接 PC9，则使用 INT2，并把 INT_MAP 的 Activity 位设为 1。

ADXL345 的 VS、VDD_IO、CS 高电平和 I2C 上拉需要在主控休眠时保持。现有设计把 ADXL345 放在常供电域；不要随 SENEN 关闭它。GPIO EXTI 可唤醒 STOP2，无须使用专用 WKUP 引脚，也无须为 PA8 调用 `HAL_PWR_EnableWakeUpPin()`。

## 3. CubeMX 逐项配置

后续实施时打开 `DeskClock.ioc`，先核对并完成 A；B 中的 LPTIM1 基础配置当前已存在，Tickless 只在实现自定义端口后启用。以下是待实施的配置步骤，本次文档更新不修改 `.ioc` 或固件。

### A. Activity 中断与基础唤醒

| 位置 | 设置 |
| --- | --- |
| Pinout，PA8 | `GPIO_EXTI8`，不配置成 MCO 或定时器复用 |
| System Core -> GPIO，PA8 | External Interrupt Mode with Rising edge；No pull；User Label=`ACC_INT1` |
| System Core -> NVIC | 启用 EXTI line[9:5] interrupts；抢占优先级 5，子优先级 0 |
| System Core -> SYS | 保留 Serial Wire；HAL Timebase Source 保持 TIM17 |
| Connectivity -> I2C2 | 保留 PB13 / PB11、400 kHz 和当前时序配置 |
| RCC / Clock Configuration | 保留运行时 HSE + PLL 的 80 MHz 配置；LSE 晶体保持开启，RTC 时钟取 LSE |
| RTC | 保留日历；需要定时刷新时由应用设置下一次 Alarm，中断优先级 5 |
| PWR | 保持支持 80 MHz 的正常运行电压配置；进入 STOP2 由代码调用，不需改成 Low-power run |
| Project Manager -> Code Generator | 保留 USER CODE；工具链仍为 MDK-ARM / Keil |

ADXL345 中断输出为推挽，下面设置为有效高，与 PA8 的上升沿匹配。PC9 若暂不使用中断，可配置为无上下拉输入；若保留 EXTI9，必须处理对应 pending，不能让共用向量中的未处理来源反复进入 ISR。其他已使用的 EXTI 引脚按各自功能保留。

生成后检查 `gpio.c` 中 `ACC_INT1_Pin=GPIO_PIN_8`、GPIOA、`GPIO_MODE_IT_RISING`，以及 NVIC 的使能。`stm32l4xx_it.c` 应有 `EXTI9_5_IRQHandler()`，里面调用 `HAL_GPIO_EXTI_IRQHandler(ACC_INT1_Pin)`。共用向量中有其他来源时一并处理；不要在独立文件重复定义 CubeMX 已生成的同名 IRQ 函数。

本工程能调用 RTOS ISR API 的抢占优先级数值为 5..15。不要给会通知任务的 EXTI 配成 0..4。

### B. FreeRTOS STOP2 的低速计时资源

建议使用 LPTIM1；STM32L471 的 LPTIM2 不能在 STOP2 中作为本方案的计时资源。

| 位置 | 设置 |
| --- | --- |
| Timers -> LPTIM1 | 启用内部计数器；Clock Source=`APB clock / low-power oscillator`，不占外部计数或输出引脚 |
| RCC 的 LPTIM1 Clock Mux | 保持当前 LSE，32768 Hz；不能改为在 STOP2 停止的 PCLK |
| LPTIM1 prescaler | 第一版可选 DIV1，16 位计数周期约 2 s；长休眠分段。以后可选 DIV32，1024 Hz，周期约 64 s |
| NVIC | 启用 LPTIM1 global interrupt；例如抢占优先级 5、子优先级 0 |
| FreeRTOS -> Config parameters | 接入自定义 Tickless 后令 `configUSE_TICKLESS_IDLE=2`；保留 RTOS 1000 Hz |

CubeMX 不同版本可能只提供 Tickless 的 Enabled/Disabled 开关。若生成的是 1，可在 `FreeRTOSConfig.h` 的 `USER CODE BEGIN Defines` 中 `#undef configUSE_TICKLESS_IDLE` 后定义为 2，并保留到重生成流程中。**只有已经实现自定义 `vPortSuppressTicksAndSleep()` 才切到 2**，否则可能缺失实现而链接失败。

当前 ARMCC 端口的默认 Tickless 实现在 `portable/RVDS/ARM_CM4F/port.c`，用 SysTick 计算睡眠时间。它适用于默认的浅睡眠流程，不能仅把 pre-sleep hook 换成 STOP2 就获得正确时间补偿。新实现应放在用户平台文件中，不修改 FreeRTOS 或 HAL 厂商文件。

## 4. ADXL345 Activity 配置

在现有 `device/imu_adxl345.c` 扩展接口，继续使用 `s_update()` 读改写并回读校验。下面采用 Activity 单独使能、AC 耦合、三轴参与，INT1 输出。寄存器值是第一轮调试配置。

| 寄存器 | 地址 | 目标配置 |
| --- | --- | --- |
| THRESH_ACT | 0x24 | 0x04，4 × 62.5 mg = 0.25 g，待实测调参 |
| ACT_INACT_CTL | 0x27 | 高四位 0xF0：AC Activity + X/Y/Z；本版不启用 Inactivity 中断 |
| BW_RATE | 0x2C | 保持 0x0A，normal power、100 Hz |
| POWER_CTL | 0x2D | 有效位保持 0x08，Measure=1，暂不启用 Link / Auto-sleep / Sleep |
| INT_ENABLE | 0x2E | 只使能 Activity 位 0x10，DATA_READY 不输出中断 |
| INT_MAP | 0x2F | Activity 位清零映射 INT1；改用 INT2 时置 0x10 |
| DATA_FORMAT | 0x31 | INT_INVERT=0，其余保持 Full Resolution、右对齐、±2 g |
| INT_SOURCE | 0x30 | 读取并保存原因；Activity 位为 0x10，读取有清锁存副作用 |

AC Activity 把检测开始时的加速度作为参考，可以避免直接把静止重力与 0.25 g 比较。参与的任意一轴超过门槛就能触发。此门槛不是三轴向量模长；Activity 没有独立的 30 ms 持续时间寄存器，不能照搬 Python 检测参数。

下面函数是添加到现有 Device 文件的示例，需同时增加头文件声明。宏必须收在 Device 文件顶部。调用前先完成 `adxl345_init()`；任务句柄和 EXTI 处理必须先准备好。此例停用该器件的其他中断，适用于当前唯一的动作唤醒用途。

```c
#define ADXL345_REG_THRESH_ACT       0x24U
#define ADXL345_REG_ACT_INACT_CTL    0x27U
#define ADXL345_REG_INT_MAP          0x2FU
#define ADXL345_ACTIVITY             0x10U
#define ADXL345_ACTIVITY_CTL_MASK    0xF0U
#define ADXL345_ACTIVITY_AC_XYZ      0xF0U
#define ADXL345_INT_INVERT           0x20U
#define ADXL345_ACTIVITY_THRESHOLD   0x04U

adxl345_status_t adxl345_arm_activity(const adxl345_ctx_t *p_ctx)
{
    adxl345_status_t result;
    uint8_t old_source;

    if (p_ctx == NULL || p_ctx->read_reg == NULL ||
        p_ctx->write_reg == NULL) {
        return ADXL345_ERROR_ARGUMENT;
    }
    result = s_update(p_ctx, ADXL345_REG_INT_ENABLE,
                      ADXL345_ALL_BITS, 0U);
    if (result != ADXL345_OK) { return result; }
    result = s_update(p_ctx, ADXL345_REG_POWER_CTL,
                      ADXL345_POWER_MASK, 0U);
    if (result != ADXL345_OK) { return result; }
    result = s_update(p_ctx, ADXL345_REG_THRESH_ACT,
                      ADXL345_ALL_BITS, ADXL345_ACTIVITY_THRESHOLD);
    if (result != ADXL345_OK) { return result; }
    result = s_update(p_ctx, ADXL345_REG_ACT_INACT_CTL,
                      ADXL345_ACTIVITY_CTL_MASK, ADXL345_ACTIVITY_AC_XYZ);
    if (result != ADXL345_OK) { return result; }
    result = s_update(p_ctx, ADXL345_REG_DATA_FORMAT,
                      ADXL345_INT_INVERT, 0U);
    if (result != ADXL345_OK) { return result; }
    result = s_update(p_ctx, ADXL345_REG_INT_MAP,
                      ADXL345_ACTIVITY, 0U);
    if (result != ADXL345_OK) { return result; }
    if (p_ctx->read_reg(p_ctx->handle, ADXL345_REG_INT_SOURCE,
                        &old_source, 1U) != 0) {
        return ADXL345_ERROR_IO;
    }
    result = s_update(p_ctx, ADXL345_REG_POWER_CTL,
                      ADXL345_POWER_MASK, ADXL345_MEASURE);
    if (result != ADXL345_OK) { return result; }
    return s_update(p_ctx, ADXL345_REG_INT_ENABLE,
                    ADXL345_ALL_BITS, ADXL345_ACTIVITY);
}
```

此函数只负责器件配置。任何步骤失败都禁止进入依赖该唤醒源的 STOP2，由应用留在运行态限频重试。进入待机前还需完成稳定等待并检查 INT 引脚、EXTI pending 和任务事件；若已经有动作，就处理它，不能再次读 INT_SOURCE 后把它静默丢掉。

## 5. 中断只通知任务

建议把当前 accelTask 改为拥有 ADXL345 的动作任务，在同一个任务内管理 ACTIVE / ARMING / WAIT_MOTION。不要另开任务同时操作同一个器件。

下面是 RTOS 运行时的通知示例。`s_motion_task` 在该任务启动时用 `xTaskGetCurrentTaskHandle()` 设置，且必须早于使能传感器输出与 EXTI。头文件需声明 `motion_irq_from_isr()`；HAL 回调放在 Platform，通知函数放在 App。

```c
/* app/motion.c: proposed names, not current repository interfaces. */
#define MOTION_EVENT_ACTIVITY (1UL << 0)
static TaskHandle_t s_motion_task;

void motion_irq_from_isr(void)
{
    BaseType_t higher_priority_woken = pdFALSE;

    if (s_motion_task != NULL) {
        (void)xTaskNotifyFromISR(s_motion_task, MOTION_EVENT_ACTIVITY,
                                eSetBits, &higher_priority_woken);
        portYIELD_FROM_ISR(higher_priority_woken);
    }
}

/* platform: use the PA8 macro generated after the CubeMX label change. */
void HAL_GPIO_EXTI_Callback(uint16_t pin)
{
    if (pin == ACC_INT1_Pin) {
        motion_irq_from_isr();
    }
}
```

等待使用 `xTaskNotifyWait(..., portMAX_DELAY)`。任务收到事件后读取并分发 INT_SOURCE，确认 Activity 后增加动作计数，再通知系统管理任务。I2C、串口、屏幕恢复和 WiFi 上电都在任务里执行。ISR 中不调用 HAL_Delay、不读 I2C、不刷新屏幕。

当前 `adxl345_sample()` 每次先读 INT_SOURCE；即使返回 NOT_READY，也可能已经清除 Activity。因此改成动作检测时，必须把 INT_SOURCE 交给一个所有者读取并分发，或修改采样接口让任何返回路径都保留事件。旧的 1 ms 遥测轮询不能与独立动作处理竞争清锁存。

Activity 锁存通过读取 INT_SOURCE 清除，STM32 EXTI pending 则由 `HAL_GPIO_EXTI_IRQHandler()` 清除，两者是不同的状态。再次睡眠前要确认引脚回到低电平。连续运动期间可暂时屏蔽 Activity 输出，由任务等待静止后重新布防，避免重复中断和立即唤醒循环。

## 6. FreeRTOS 如何进入 STOP2

由系统管理任务准备休眠，由 RTOS Idle / 自定义 Tickless 端口执行最后的 STOP2 入口。不要在普通任务里直接停掉节拍并无限期睡眠，也不要只调用 `osKernelLock()` 就认为时间和任务已经处理正确。

进入待机之前，业务任务完成以下协作：

1. 停止旧 accelTask 的 1 ms 轮询；动作任务进入事件阻塞。
2. 停止连续 JustFloat 遥测，等待 debug 队列及 UART 最后一个停止位发完。低功耗期间不发送 100 Hz 帧。
3. 显示任务完成当前刷新和 BUSY 等待，再由其所有者让屏幕休眠。当前生产页面按分钟全刷，但显示任务每 200 ms 检查快照；低功耗整合时需改为事件和 RTC 期限驱动，单靠少刷新不能进入长待机。
4. 当前 defaultTask 每秒通过 `vTaskDelayUntil()` 运行时钟服务，需与 RTC/低功耗计时统一安排。其他任务、后台 Flash 扫描和软件定时器也要纳入最早唤醒期限。
5. 需要关闭电源域时，由相应业务所有者完成 WiFi / 传感器停机；确认没有 I2C/SPI/UART/DMA 事务或持有的总线锁。墨水屏保持供电，ADXL345 与 RTC 保留。
6. 布防 Activity，记录已发生事件并检查 PA8 为低；有事件或准备失败就取消睡眠。

以下是自定义 `vPortSuppressTicksAndSleep()` 的流程伪代码，**不是可直接编译的完整端口**：

```text
Idle 已挂起调度，传入 xExpectedIdleTime
    保存中断屏蔽状态，短暂屏蔽中断，复查 eTaskConfirmSleepModeStatus()
    若有任务要运行、动作事件、EXTI pending 或 PA8 为高，取消入口
    保存 SysTick 当前相位并停止 SysTick；暂停 TIM17 的 HAL tick
    用 LPTIM1/LSE 记录起点，按最早 RTOS 期限设置有限唤醒比较
    重新检查入口条件；保留这一过程中新到达的中断 pending
    DSB
    HAL_PWREx_EnterSTOP2Mode(PWR_STOPENTRY_WFI)
    根据 LPTIM1/RTC 取得经过的低速时钟计数
    在任务和普通外设 ISR 执行前，恢复运行时钟及 SystemCoreClock
    按经过时间补偿 RTOS tick，处理到期节拍并恢复 SysTick 相位
    恢复 TIM17、必要的 HAL 时间和外设状态
    恢复进入函数时的中断屏蔽状态，返回 Idle
唤醒中断通知动作任务；调度器恢复后，任务处理动作原因
```

关键实现要求：

- WFI 前的条件检查与休眠必须防止检查后发生的事件被漏掉。已使能的中断 pending 可以阻止 WFI 真正睡下；不能在最后检查后盲清 EXTI/NVIC pending。
- SysTick 在 STOP2 不计时。用低速计数计算实际睡眠时长，保留除法余数和不足一个 tick 的相位。DIV32 的 1024 Hz 与 RTOS 的 1000 Hz 不能当作同一个频率。
- LPTIM1 只有 16 位计数，处理计数回绕与比较值写入同步；有限睡眠必须在可判别的跨度内，长睡眠用分段或 RTC。不能只取一次计数差就忽略多次回绕。
- 提前被 Activity 唤醒时，只补偿实际经过时间；到达任务期限时，遵守 FreeRTOS `vTaskStepTick()` 的边界，并通过端口的最后一个 tick 处理或等价路径解除到期任务。不能跨过最近期限，也不能指望 StepTick 本身完成所有到期处理。
- HAL tick 与 RTOS tick 是两套时间。当前 TIM17 在 STOP2 停止，`HAL_SuspendTick()` 只控制 TIM17 中断，不会关闭 RTOS SysTick。RTC 日历用于真实时间；需要跨休眠连续的毫秒时间时必须另行定义和补偿。
- 唤醒先运行 MSI 或 HSI16，原有 HSE/PLL 80 MHz 不会自动恢复。若在中断被屏蔽且 HAL tick 已暂停时调用当前 `SystemClock_Config()`，其中依赖 HAL_GetTick 的超时可能失去作用。正式端口需要有独立超时的时钟恢复路径，再恢复节拍与调度。
- 若选择先在临时时钟下开放部分中断以恢复 HAL 超时，必须阻止 PendSV/任务运行和普通外设访问，直到时钟恢复完成。不要在所有中断关着时等待 TIM17 tick。
- 时钟恢复到原频率后，再使用 I2C2 和 USART2；若有实际丢失的配置、外设掉电或忙状态，则按所有者恢复。不要每次 STOP2 唤醒都重新调用 MX_RTC_Init 或清掉保留日历。

## 7. 推荐接入顺序与代码归属

| 阶段 | 代码位置 | 可观察结果 |
| --- | --- | --- |
| 先验证 Activity | `device/imu_adxl345.*` 扩展布防/原因读取，Platform EXTI 回调，现有 accelTask 处理事件 | 主控正常运行时，拿起后 PA8 上升，任务动作计数增加；静止不持续重触发 |
| 再验证一次 STOP2 | 单独测试入口放 Platform，从 `main.c` USER CODE、RTOS 启动前调用 | 无其他业务任务时睡下，动作后恢复到调用后的代码；ISR 只记标志，此阶段不调用 RTOS API |
| 最后整合 RTOS | `app/power.*` 管业务准备，`platform/stm32_low_power.*` 实现自定义 Tickless/时钟/计时 | 所有任务阻塞后进入 STOP2；动作、RTC 或最早期限唤醒，任务与时间正常恢复 |

第二阶段的裸机试验要单独处理 SysTick、TIM17、其他中断来源及有限时钟恢复；不能把两行 HAL 调用直接当成完整 RTOS 实现。第三阶段开始前先让显示、采样和 debug 任务具有明确的休眠准备/恢复接口。生成文件只保留 USER CODE 调用，新增 C 文件同步加入 Keil 工程。

## 8. 验收方法

先验证运行态的中断计数，再测 STOP2：静止后电流下降、动作后电流和任务恢复、再次静止可以重新睡下。记录动作开始/中断/任务恢复时刻、INT_SOURCE、唤醒原因和 RTC 时间。重复不少于一组人工可核对次数的拿起/放下，并加上缓慢拿起、敲桌和持续移动测试。

计时验证至少包括 Activity 提前唤醒、定时器到期、计数回绕和重复进出 STOP2。注入 I2C 配置失败、INT 脚持续为高、入睡瞬间到达中断，检查能取消睡眠或处理事件。唤醒后复测 I2C/串口速率、屏幕任务和 RTC 日历。

测功耗时单独记录调试器连接、DBG_STOP、LED、USB-UART 连接和外部模块状态。ST-Link 调试保持可能增加电流；USB-UART 等外部接口也可能影响测量。保持 SWD 接线与可恢复下载方式，实际待机电流用电流表确认，不能把芯片手册的 MCU 电流直接当成整板电流。

## 9. 依据

- `datasheet/主控板资料/加速度计.pdf`，ADXL345 Rev. E，印刷页 20、24..27：中断推挽输出、Activity/AC 比较、门槛单位、INT_MAP、INT_SOURCE、Measure/Sleep。
- `datasheet/主控板资料/STM32L471RET6_规格书_WJ95548.PDF`，DocID027226 Rev 1，页 21..29、44、47：STOP2 保留内容、GPIO/RTC 唤醒、SysTick 停止、LPTIM1 与 LPTIM2 差异、I2C2 在 STOP2 中不可运行。
- 本工程 `Drivers/STM32L4xx_HAL_Driver/Src/stm32l4xx_hal_pwr_ex.c` 的 `HAL_PWREx_EnterSTOP2Mode()` 与本地 RCC HAL 实现。
- 本工程 `Middlewares/Third_Party/FreeRTOS/Source/portable/RVDS/ARM_CM4F/port.c` 的默认 Tickless 实现、`tasks.c` 的 Idle/StepTick 处理。
- 当前 `Core/Inc/FreeRTOSConfig.h`、`Core/Src/stm32l4xx_hal_timebase_tim.c`、`Core/Src/freertos.c`、`app/accel.c`、`device/imu_adxl345.c` 和历史网表。
