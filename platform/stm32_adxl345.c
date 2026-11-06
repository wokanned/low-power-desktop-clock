#include "stm32_adxl345.h"
#include "i2c.h"
#include "main.h"
#include "FreeRTOS.h"
#include "semphr.h"

#define ADXL345_BUS_TIMEOUT_MS 1U
#define ADXL345_BUS_ATTEMPTS 2U
#define ADXL345_BUS_READ_ATTEMPTS 1U
#define ADXL345_BUS_LOCK_MS 2U
#define ADXL345_MOTION_IRQ_PRIORITY 5U

typedef struct {
    I2C_HandleTypeDef *i2c;
    SemaphoreHandle_t mutex;
} adxl345_bus_t;
static StaticSemaphore_t s_mutex_control;
static adxl345_bus_t s_bus;
static stm32_adxl345_motion_notify_t s_motion_notify;
static volatile uint8_t s_motion_pending;
static volatile uint8_t s_motion_enabled;

void stm32_adxl345_motion_init(stm32_adxl345_motion_notify_t notify)
{
    stm32_adxl345_motion_disarm();
    s_motion_notify = notify;
    HAL_NVIC_SetPriority(EXTI9_5_IRQn, ADXL345_MOTION_IRQ_PRIORITY, 0U);
    HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);
}

void stm32_adxl345_motion_disarm(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    CLEAR_BIT(EXTI->IMR1, ACCINT2A8_Pin);
    s_motion_enabled = 0U;
    __set_PRIMASK(primask);
}

void stm32_adxl345_motion_take_pending(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    CLEAR_BIT(EXTI->IMR1, ACCINT2A8_Pin);
    s_motion_enabled = 0U;
    s_motion_pending = 0U;
    __HAL_GPIO_EXTI_CLEAR_IT(ACCINT2A8_Pin);
    __set_PRIMASK(primask);
}

uint8_t stm32_adxl345_motion_high(void)
{
    return (uint8_t)(HAL_GPIO_ReadPin(ACCINT2A8_GPIO_Port, ACCINT2A8_Pin) == GPIO_PIN_SET);
}

uint8_t stm32_adxl345_motion_arm(void)
{
    uint32_t primask = __get_PRIMASK();
    uint8_t armed = 0U;
    __disable_irq();
    if (stm32_adxl345_motion_high() == 0U && s_motion_pending == 0U &&
        __HAL_GPIO_EXTI_GET_IT(ACCINT2A8_Pin) == 0U) {
        s_motion_enabled = 1U;
        SET_BIT(EXTI->IMR1, ACCINT2A8_Pin);
        armed = 1U;
    }
    __set_PRIMASK(primask);
    return armed;
}

void stm32_adxl345_motion_exti_callback(uint16_t pin)
{
    if (pin != ACCINT2A8_Pin) { return; }
    /* The shared vector may service PC9 while EXTI8 is masked. Preserve the
       event, but notify at most once until the owner explicitly rearms. */
    s_motion_pending = 1U;
    if (s_motion_enabled != 0U) {
        CLEAR_BIT(EXTI->IMR1, ACCINT2A8_Pin);
        s_motion_enabled = 0U;
        if (s_motion_notify != NULL) { s_motion_notify(); }
    }
}

static int32_t s_transfer(void *p_handle, uint8_t reg, uint8_t *p_data,
                          uint16_t len, uint8_t write)
{
    adxl345_bus_t *p_bus = (adxl345_bus_t *)p_handle;
    HAL_StatusTypeDef result = HAL_ERROR;
    uint8_t attempt;
    uint8_t attempts = write != 0U ? ADXL345_BUS_ATTEMPTS : ADXL345_BUS_READ_ATTEMPTS;
    if (p_bus == NULL || p_data == NULL || len == 0U) { return -1; }
    if (xSemaphoreTake(p_bus->mutex, pdMS_TO_TICKS(ADXL345_BUS_LOCK_MS)) != pdTRUE) {
        return -1;
    }
    /* Reads can acknowledge INT_SOURCE. Retrying an ambiguous read could hide
       an activity that the first transaction already cleared; let the owner
       see the failure and veto sleep until it has re-established the source. */
    for (attempt = 0U; attempt < attempts; attempt++) {
        if (write != 0U) {
            result = HAL_I2C_Mem_Write(p_bus->i2c, ADXL345_ADDRESS_7BIT << 1U,
                reg, I2C_MEMADD_SIZE_8BIT, p_data, len, ADXL345_BUS_TIMEOUT_MS);
        } else {
            result = HAL_I2C_Mem_Read(p_bus->i2c, ADXL345_ADDRESS_7BIT << 1U,
                reg, I2C_MEMADD_SIZE_8BIT, p_data, len, ADXL345_BUS_TIMEOUT_MS);
        }
        if (result == HAL_OK) { break; }
    }
    (void)xSemaphoreGive(p_bus->mutex);
    return (result == HAL_OK) ? 0 : -1;
}

static int32_t s_read(void *p_handle, uint8_t reg, uint8_t *p_data, uint16_t len)
{
    return s_transfer(p_handle, reg, p_data, len, 0U);
}
static int32_t s_write(void *p_handle, uint8_t reg, const uint8_t *p_data, uint16_t len)
{
    return s_transfer(p_handle, reg, (uint8_t *)p_data, len, 1U);
}
int32_t stm32_adxl345_bind(adxl345_ctx_t *p_ctx)
{
    if (p_ctx == NULL) { return -1; }
    if (s_bus.mutex == NULL) {
        s_bus.mutex = xSemaphoreCreateMutexStatic(&s_mutex_control);
    }
    if (s_bus.mutex == NULL) { return -1; }
    s_bus.i2c = &hi2c2;
    p_ctx->handle = &s_bus;
    p_ctx->read_reg = s_read;
    p_ctx->write_reg = s_write;
    return 0;
}
