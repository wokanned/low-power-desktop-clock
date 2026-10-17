#include "imu_adxl345.h"
#include "vofa.h"
#include "accel.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t registers[64];
    uint8_t fail;
    uint8_t corrupt_format;
    uint32_t reads;
    uint32_t bursts;
    uint32_t writes;
} mock_bus_t;

static uint8_t s_wire[136];
static uint16_t s_wire_len;
static uint32_t s_send_calls;
static bool s_send_fail;

static int32_t s_read(void *p_handle, uint8_t reg, uint8_t *p_data, uint16_t len)
{
    mock_bus_t *p_bus = (mock_bus_t *)p_handle;
    p_bus->reads++;
    if (p_bus->fail != 0U) { return -1; }
    assert((uint16_t)reg + len <= sizeof(p_bus->registers));
    memcpy(p_data, p_bus->registers + reg, len);
    if (reg == 0x32U) {
        assert(len == 6U);
        p_bus->bursts++;
        p_bus->registers[0x30U] = 0U;
    }
    return 0;
}

static int32_t s_write(void *p_handle, uint8_t reg, const uint8_t *p_data, uint16_t len)
{
    mock_bus_t *p_bus = (mock_bus_t *)p_handle;
    p_bus->writes++;
    if (p_bus->fail != 0U) { return -1; }
    assert((uint16_t)reg + len <= sizeof(p_bus->registers));
    memcpy(p_bus->registers + reg, p_data, len);
    if (reg == 0x31U && p_bus->corrupt_format != 0U) {
        p_bus->registers[reg] ^= 0x08U;
    }
    return 0;
}

static bool s_send(void *p_handle, const uint8_t *p_data, uint16_t len)
{
    (void)p_handle;
    s_send_calls++;
    if (s_send_fail) { return false; }
    assert(s_wire_len + len <= sizeof(s_wire));
    memcpy(s_wire + s_wire_len, p_data, len);
    s_wire_len = (uint16_t)(s_wire_len + len);
    return true;
}

static void s_driver_tests(void)
{
    mock_bus_t bus;
    adxl345_ctx_t ctx = {s_read, s_write, &bus};
    adxl345_sample_t sample;
    uint32_t reads;
    memset(&bus, 0, sizeof(bus));
    bus.registers[0] = 0xE5U;
    bus.registers[0x31U] = 0xF7U;
    bus.registers[0x2DU] = 0xFFU;
    bus.registers[0x2CU] = 0xFFU;
    bus.registers[0x38U] = 0xFFU;
    bus.registers[0x2EU] = 0xFFU;
    bus.registers[0x1EU] = 42U;
    assert(adxl345_init(&ctx) == ADXL345_OK);
    assert(bus.registers[0x31U] == 0x18U); /* Preserve reserved bit 4. */
    assert(bus.registers[0x2CU] == 0xEAU);
    assert(bus.registers[0x2DU] == 0xC8U);
    assert(bus.registers[0x38U] == 0U && bus.registers[0x2EU] == 0U);
    assert(bus.registers[0x1EU] == 0U);
    bus.registers[0x30U] = ADXL345_DATA_READY | ADXL345_OVERRUN;
    bus.registers[0x32U] = 0x00U; bus.registers[0x33U] = 0xFFU;
    bus.registers[0x34U] = 0xFFU; bus.registers[0x35U] = 0x7FU;
    bus.registers[0x36U] = 0x00U; bus.registers[0x37U] = 0x80U;
    assert(adxl345_sample(&ctx, &sample) == ADXL345_OK);
    assert(sample.raw[0] == -256 && sample.raw[1] == 32767 && sample.raw[2] == -32768);
    assert(sample.source == (ADXL345_DATA_READY | ADXL345_OVERRUN));
    assert(bus.bursts == 1U);
    reads = bus.reads;
    assert(adxl345_sample(&ctx, &sample) == ADXL345_NOT_READY);
    assert(bus.reads == reads + 1U && bus.bursts == 1U);
    bus.fail = 1U;
    assert(adxl345_sample(&ctx, &sample) == ADXL345_ERROR_IO);
    assert(adxl345_init(&ctx) == ADXL345_ERROR_IO);
    bus.fail = 0U; bus.registers[0] = 0x00U;
    assert(adxl345_init(&ctx) == ADXL345_ERROR_ID);
    bus.registers[0] = 0xE5U; bus.corrupt_format = 1U;
    assert(adxl345_init(&ctx) == ADXL345_ERROR_CONFIG);
    assert((bus.registers[0x2DU] & 0x08U) == 0U);
    assert(adxl345_init(NULL) == ADXL345_ERROR_ARGUMENT);
    assert(adxl345_sample(&ctx, NULL) == ADXL345_ERROR_ARGUMENT);
}

static void s_wire_tests(const char *p_fixture)
{
    VOFA_HandleTypeDef vofa;
    VOFA_Data_TypeDef_Enum types[16];
    float values[16] = {100.0f, 0.0f, -256.0f, 0.0f, 256.0f};
    uint32_t nan_bits = 0x7FC00000U;
    uint32_t inf_bits = 0x7F800000U;
    uint8_t i;
    FILE *p_file;
    uint8_t packed[6] = {0U};
    int16_t signed16 = -2;
    uint32_t unsigned32 = 3U;
    VOFA_Data_TypeDef_Enum mixed[2] = {VOFA_DATA_SHORT, VOFA_DATA_UINT};
    VOFA_Init(&vofa, NULL, s_send, VOFA_FMT_FIREFIREWATER, VOFA_BAUD_115200);
    assert(vofa.fmt == VOFA_FMT_JUSTFLOAT);
    for (i = 0U; i < 16U; i++) { types[i] = VOFA_DATA_FLOAT; }
    for (i = 0U; i < 3U; i++) {
        values[5U + i] = values[2U + i] * ADXL345_SCALE_G_PER_LSB;
        values[8U + i] = values[5U + i];
    }
    memcpy(&values[11], &nan_bits, sizeof(nan_bits));
    values[15] = (float)(ACCEL_STATUS_INITIALIZED | ACCEL_STATUS_VALID | ACCEL_STATUS_TEMPERATURE_INVALID);
    assert(VOFA_SendData(&vofa, values, 16U, types));
    assert(s_send_calls == 1U && s_wire_len == 68U);
    assert(memcmp(s_wire + 64U, "\x00\x00\x80\x7f", 4U) == 0);
    for (i = 2U; i < 12U; i++) { memcpy(&values[i], &nan_bits, sizeof(nan_bits)); }
    values[0] = 110.0f; values[1] = 1.0f;
    values[12] = 1.0f;
    values[15] = (float)(ACCEL_STATUS_TEMPERATURE_INVALID | ACCEL_STATUS_IO_ERROR);
    assert(VOFA_SendData(&vofa, values, 16U, types));
    assert(s_wire_len == 136U);
    p_file = fopen(p_fixture, "wb");
    assert(p_file != NULL);
    assert(fwrite(s_wire, 1U, sizeof(s_wire), p_file) == sizeof(s_wire));
    assert(fclose(p_file) == 0);
    s_wire_len = 0U;
    memcpy(packed, &signed16, sizeof(signed16));
    memcpy(packed + 2U, &unsigned32, sizeof(unsigned32));
    assert(VOFA_SendData(&vofa, packed, 2U, mixed));
    assert(s_wire_len == 12U);
    assert(memcmp(s_wire, "\x00\x00\x00\xc0\x00\x00\x40\x40", 8U) == 0);
    s_wire_len = 0U;
    memcpy(&values[0], &inf_bits, sizeof(inf_bits));
    assert(!VOFA_SendData(&vofa, values, 16U, types));
    assert(s_wire_len == 0U);
    assert(!VOFA_SendData(&vofa, values, 17U, types));
    assert(!VOFA_SendData(&vofa, NULL, 16U, types));
    values[0] = 120.0f;
    s_send_fail = true;
    assert(!VOFA_SendData(&vofa, values, 16U, types));
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    s_driver_tests();
    s_wire_tests(argv[1]);
    puts("PASS: ADXL345 ID/config/burst/sign/error handling and JustFloat C encoder");
    return 0;
}
