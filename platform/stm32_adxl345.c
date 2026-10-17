#include "stm32_adxl345.h"
#include "i2c.h"
#include "FreeRTOS.h"
#include "semphr.h"

#define ADXL345_BUS_TIMEOUT_MS 1U
#define ADXL345_BUS_ATTEMPTS 2U
#define ADXL345_BUS_LOCK_MS 2U

typedef struct {
    I2C_HandleTypeDef *i2c;
    SemaphoreHandle_t mutex;
} adxl345_bus_t;
static StaticSemaphore_t s_mutex_control;
static adxl345_bus_t s_bus;

static int32_t s_transfer(void *p_handle, uint8_t reg, uint8_t *p_data,
                          uint16_t len, uint8_t write)
{
    adxl345_bus_t *p_bus = (adxl345_bus_t *)p_handle;
    HAL_StatusTypeDef result = HAL_ERROR;
    uint8_t attempt;
    if (p_bus == NULL || p_data == NULL || len == 0U) { return -1; }
    if (xSemaphoreTake(p_bus->mutex, pdMS_TO_TICKS(ADXL345_BUS_LOCK_MS)) != pdTRUE) {
        return -1;
    }
    for (attempt = 0U; attempt < ADXL345_BUS_ATTEMPTS; attempt++) {
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
