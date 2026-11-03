#include "imu_adxl345.h"
#include <stddef.h>

#define ADXL345_REG_DEVID        0x00U
#define ADXL345_ID               0xE5U
#define ADXL345_REG_OFSX         0x1EU
#define ADXL345_REG_THRESH_ACT   0x24U
#define ADXL345_REG_ACT_INACT_CTL 0x27U
#define ADXL345_REG_BW_RATE      0x2CU
#define ADXL345_REG_POWER_CTL    0x2DU
#define ADXL345_REG_INT_ENABLE   0x2EU
#define ADXL345_REG_INT_MAP      0x2FU
#define ADXL345_REG_INT_SOURCE   0x30U
#define ADXL345_REG_DATA_FORMAT  0x31U
#define ADXL345_REG_DATAX0       0x32U
#define ADXL345_REG_FIFO_CTL     0x38U
#define ADXL345_POWER_MASK       0x3FU
#define ADXL345_MEASURE          0x08U
#define ADXL345_RATE_MASK        0x1FU
#define ADXL345_RATE_100HZ       0x0AU
#define ADXL345_FORMAT_MASK      0xEFU
#define ADXL345_FULL_RES         0x08U
#define ADXL345_ALL_BITS         0xFFU
#define ADXL345_AXIS_COUNT       3U
#define ADXL345_DATA_BYTES       6U
#define ADXL345_ACTIVITY_CTL_MASK 0xF0U
#define ADXL345_ACTIVITY_AC_XYZ   0xF0U
#define ADXL345_ACTIVITY_THRESHOLD 0x04U
#define ADXL345_INT_INVERT        0x20U

static adxl345_status_t s_update(const adxl345_ctx_t *p_ctx,
                                 uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t current;
    uint8_t verified;
    if (p_ctx->read_reg(p_ctx->handle, reg, &current, 1U) != 0) {
        return ADXL345_ERROR_IO;
    }
    current = (uint8_t)((current & (uint8_t)~mask) | (value & mask));
    if (p_ctx->write_reg(p_ctx->handle, reg, &current, 1U) != 0 ||
        p_ctx->read_reg(p_ctx->handle, reg, &verified, 1U) != 0) {
        return ADXL345_ERROR_IO;
    }
    return ((verified & mask) == (current & mask)) ?
        ADXL345_OK : ADXL345_ERROR_CONFIG;
}

adxl345_status_t adxl345_init(const adxl345_ctx_t *p_ctx)
{
    uint8_t id;
    uint8_t i;
    adxl345_status_t result;
    if (p_ctx == NULL || p_ctx->read_reg == NULL || p_ctx->write_reg == NULL) {
        return ADXL345_ERROR_ARGUMENT;
    }
    if (p_ctx->read_reg(p_ctx->handle, ADXL345_REG_DEVID, &id, 1U) != 0) {
        return ADXL345_ERROR_IO;
    }
    if (id != ADXL345_ID) {
        return ADXL345_ERROR_ID;
    }
    /* ADXL345 has no software reset. Re-establish a known state in standby. */
    result = s_update(p_ctx, ADXL345_REG_POWER_CTL, ADXL345_POWER_MASK, 0U);
    if (result != ADXL345_OK) { return result; }
    result = s_update(p_ctx, ADXL345_REG_INT_ENABLE, ADXL345_ALL_BITS, 0U);
    if (result != ADXL345_OK) { return result; }
    result = s_update(p_ctx, ADXL345_REG_FIFO_CTL, ADXL345_ALL_BITS, 0U);
    if (result != ADXL345_OK) { return result; }
    result = s_update(p_ctx, ADXL345_REG_DATA_FORMAT, ADXL345_FORMAT_MASK, ADXL345_FULL_RES);
    if (result != ADXL345_OK) { return result; }
    result = s_update(p_ctx, ADXL345_REG_BW_RATE, ADXL345_RATE_MASK, ADXL345_RATE_100HZ);
    if (result != ADXL345_OK) { return result; }
    for (i = 0U; i < ADXL345_AXIS_COUNT; i++) {
        result = s_update(p_ctx, (uint8_t)(ADXL345_REG_OFSX + i), ADXL345_ALL_BITS, 0U);
        if (result != ADXL345_OK) { return result; }
    }
    return s_update(p_ctx, ADXL345_REG_POWER_CTL, ADXL345_POWER_MASK, ADXL345_MEASURE);
}

adxl345_status_t adxl345_read_interrupt(const adxl345_ctx_t *p_ctx, uint8_t *p_source)
{
    uint8_t source;
    if (p_source != NULL) { *p_source = 0U; }
    if (p_ctx == NULL || p_ctx->read_reg == NULL || p_source == NULL) {
        return ADXL345_ERROR_ARGUMENT;
    }
    if (p_ctx->read_reg(p_ctx->handle, ADXL345_REG_INT_SOURCE, &source, 1U) != 0) {
        return ADXL345_ERROR_IO;
    }
    *p_source = source;
    return ADXL345_OK;
}

adxl345_status_t adxl345_arm_activity(const adxl345_ctx_t *p_ctx, uint8_t *p_source)
{
    adxl345_status_t result;
    if (p_source != NULL) { *p_source = 0U; }
    if (p_ctx == NULL || p_ctx->read_reg == NULL || p_ctx->write_reg == NULL ||
        p_source == NULL) { return ADXL345_ERROR_ARGUMENT; }
    result = s_update(p_ctx, ADXL345_REG_INT_ENABLE, ADXL345_ALL_BITS, 0U);
    if (result != ADXL345_OK) { return result; }
    result = s_update(p_ctx, ADXL345_REG_POWER_CTL, ADXL345_POWER_MASK, 0U);
    if (result != ADXL345_OK) { return result; }
    result = s_update(p_ctx, ADXL345_REG_THRESH_ACT, ADXL345_ALL_BITS,
                      ADXL345_ACTIVITY_THRESHOLD);
    if (result != ADXL345_OK) { return result; }
    result = s_update(p_ctx, ADXL345_REG_ACT_INACT_CTL, ADXL345_ACTIVITY_CTL_MASK,
                      ADXL345_ACTIVITY_AC_XYZ);
    if (result != ADXL345_OK) { return result; }
    result = s_update(p_ctx, ADXL345_REG_DATA_FORMAT, ADXL345_INT_INVERT, 0U);
    if (result != ADXL345_OK) { return result; }
    result = s_update(p_ctx, ADXL345_REG_INT_MAP, ADXL345_ACTIVITY, 0U);
    if (result != ADXL345_OK) { return result; }
    result = adxl345_read_interrupt(p_ctx, p_source);
    if (result != ADXL345_OK) { return result; }
    result = s_update(p_ctx, ADXL345_REG_POWER_CTL, ADXL345_POWER_MASK, ADXL345_MEASURE);
    if (result != ADXL345_OK) { return result; }
    return s_update(p_ctx, ADXL345_REG_INT_ENABLE, ADXL345_ALL_BITS, ADXL345_ACTIVITY);
}

adxl345_status_t adxl345_check_activity(const adxl345_ctx_t *p_ctx)
{
    static const uint8_t registers[] = { ADXL345_REG_DEVID, ADXL345_REG_BW_RATE,
        ADXL345_REG_POWER_CTL, ADXL345_REG_DATA_FORMAT, ADXL345_REG_THRESH_ACT,
        ADXL345_REG_ACT_INACT_CTL, ADXL345_REG_INT_MAP, ADXL345_REG_INT_ENABLE };
    static const uint8_t masks[] = { ADXL345_ALL_BITS, ADXL345_RATE_MASK,
        ADXL345_POWER_MASK, ADXL345_FORMAT_MASK, ADXL345_ALL_BITS,
        ADXL345_ACTIVITY_CTL_MASK, ADXL345_ACTIVITY, ADXL345_ALL_BITS };
    static const uint8_t values[] = { ADXL345_ID, ADXL345_RATE_100HZ,
        ADXL345_MEASURE, ADXL345_FULL_RES, ADXL345_ACTIVITY_THRESHOLD,
        ADXL345_ACTIVITY_AC_XYZ, 0U, ADXL345_ACTIVITY };
    uint8_t value;
    uint8_t i;
    if (p_ctx == NULL || p_ctx->read_reg == NULL) { return ADXL345_ERROR_ARGUMENT; }
    for (i = 0U; i < sizeof(registers); i++) {
        if (p_ctx->read_reg(p_ctx->handle, registers[i], &value, 1U) != 0) {
            return ADXL345_ERROR_IO;
        }
        if ((value & masks[i]) != values[i]) {
            return i == 0U ? ADXL345_ERROR_ID : ADXL345_ERROR_CONFIG;
        }
    }
    return ADXL345_OK;
}

adxl345_status_t adxl345_sample(const adxl345_ctx_t *p_ctx, adxl345_sample_t *p_sample)
{
    uint8_t bytes[ADXL345_DATA_BYTES];
    uint8_t source;
    uint8_t i;
    int32_t decoded;
    if (p_ctx == NULL || p_ctx->read_reg == NULL || p_sample == NULL) {
        return ADXL345_ERROR_ARGUMENT;
    }
    p_sample->source = 0U;
    if (adxl345_read_interrupt(p_ctx, &source) != ADXL345_OK) {
        return ADXL345_ERROR_IO;
    }
    p_sample->source = source;
    if ((source & ADXL345_DATA_READY) == 0U) {
        return ADXL345_NOT_READY;
    }
    if (p_ctx->read_reg(p_ctx->handle, ADXL345_REG_DATAX0, bytes, sizeof(bytes)) != 0) {
        return ADXL345_ERROR_IO;
    }
    for (i = 0U; i < ADXL345_AXIS_COUNT; i++) {
        decoded = (int32_t)((uint16_t)bytes[i * 2U] |
                           ((uint16_t)bytes[i * 2U + 1U] << 8U));
        if ((decoded & 0x8000L) != 0L) { decoded -= 0x10000L; }
        p_sample->raw[i] = (int16_t)decoded;
    }
    return ADXL345_OK;
}
