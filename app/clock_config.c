#include "clock_config.h"
#include <string.h>

#define CONFIG_MAGIC 0x314B4C43UL
#define CONFIG_VERSION 1UL
#define CONFIG_COMMIT 0x54494D43UL
#define CONFIG_BODY_SIZE 20U
#define CONFIG_CRC_OFFSET 16U
#define CONFIG_COMMIT_OFFSET 20U
#define CONFIG_CRC_POLYNOMIAL 0xEDB88320UL

static uint32_t s_get_u32(const uint8_t *p_bytes)
{
    return (uint32_t)p_bytes[0] | ((uint32_t)p_bytes[1] << 8U) |
           ((uint32_t)p_bytes[2] << 16U) | ((uint32_t)p_bytes[3] << 24U);
}

static void s_put_u32(uint8_t *p_bytes, uint32_t value)
{
    p_bytes[0] = (uint8_t)value;
    p_bytes[1] = (uint8_t)(value >> 8U);
    p_bytes[2] = (uint8_t)(value >> 16U);
    p_bytes[3] = (uint8_t)(value >> 24U);
}

static uint32_t s_crc(const uint8_t *p_bytes, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFFUL;
    uint32_t index;
    uint32_t bit;
    for (index = 0U; index < length; index++) {
        crc ^= p_bytes[index];
        for (bit = 0U; bit < 8U; bit++) {
            crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? CONFIG_CRC_POLYNOMIAL : 0U);
        }
    }
    return ~crc;
}

static uint8_t s_erased(const uint8_t *p_bytes)
{
    uint32_t index;
    for (index = 0U; index < CLOCK_CONFIG_RECORD_SIZE; index++) {
        if (p_bytes[index] != 0xFFU) { return 0U; }
    }
    return 1U;
}

static uint8_t s_valid(const uint8_t *p_bytes)
{
    uint32_t format = s_get_u32(&p_bytes[12]);
    return s_get_u32(p_bytes) == CONFIG_MAGIC &&
           s_get_u32(&p_bytes[4]) == CONFIG_VERSION &&
           (format == CLOCK_FORMAT_12 || format == CLOCK_FORMAT_24) &&
           s_get_u32(&p_bytes[CONFIG_CRC_OFFSET]) == s_crc(p_bytes, CONFIG_CRC_OFFSET) &&
           s_get_u32(&p_bytes[CONFIG_COMMIT_OFFSET]) == CONFIG_COMMIT;
}

int32_t clock_config_load(const w25q128_ctx_t *p_context,
                          clock_config_state_t *p_state)
{
    uint8_t records[2][CLOCK_CONFIG_RECORD_SIZE];
    uint8_t valid[2];
    uint8_t slot;
    uint32_t sequence_a;
    uint32_t sequence_b;
    int32_t result;
    if (p_context == NULL || p_state == NULL) { return W25Q128_ERROR_ARGUMENT; }
    p_state->format = CLOCK_FORMAT_24;
    p_state->sequence = 0U;
    p_state->active_slot = CLOCK_CONFIG_NO_SLOT;
    p_state->valid = 0U;
    p_state->degraded = 0U;
    result = w25q128_read(p_context, CLOCK_CONFIG_SECTOR_A, records[0], sizeof(records[0]));
    if (result != W25Q128_OK) { return result; }
    result = w25q128_read(p_context, CLOCK_CONFIG_SECTOR_B, records[1], sizeof(records[1]));
    if (result != W25Q128_OK) { return result; }
    valid[0] = s_valid(records[0]);
    valid[1] = s_valid(records[1]);
    if (valid[0] == 0U && valid[1] == 0U) {
        return s_erased(records[0]) != 0U && s_erased(records[1]) != 0U ?
            W25Q128_OK : CLOCK_CONFIG_ERROR_CORRUPT;
    }
    slot = valid[0] != 0U ? 0U : 1U;
    if (valid[0] != 0U && valid[1] != 0U) {
        sequence_a = s_get_u32(&records[0][8]);
        sequence_b = s_get_u32(&records[1][8]);
        /* Adjacent successful commits differ by one, including UINT32 wrap. */
        if (sequence_b != sequence_a && (sequence_b - sequence_a) < 0x80000000UL) {
            slot = 1U;
        }
    }
    p_state->format = (clock_format_t)s_get_u32(&records[slot][12]);
    p_state->sequence = s_get_u32(&records[slot][8]);
    p_state->active_slot = slot;
    p_state->valid = 1U;
    p_state->degraded = valid[1U - slot] == 0U && s_erased(records[1U - slot]) == 0U;
    return W25Q128_OK;
}

int32_t clock_config_save(const w25q128_ctx_t *p_context,
                          clock_config_state_t *p_state, clock_format_t format)
{
    uint8_t record[CLOCK_CONFIG_RECORD_SIZE];
    uint8_t verify[CLOCK_CONFIG_RECORD_SIZE];
    uint8_t slot;
    uint32_t address;
    uint32_t sequence;
    int32_t result;
    if (p_context == NULL || p_state == NULL ||
        (format != CLOCK_FORMAT_12 && format != CLOCK_FORMAT_24) ||
        (p_state->valid != 0U && p_state->active_slot > 1U)) {
        return W25Q128_ERROR_ARGUMENT;
    }
    if (p_state->valid != 0U && p_state->format == format) { return W25Q128_OK; }
    slot = p_state->valid != 0U ? (uint8_t)(1U - p_state->active_slot) : 0U;
    address = slot == 0U ? CLOCK_CONFIG_SECTOR_A : CLOCK_CONFIG_SECTOR_B;
    sequence = p_state->sequence + 1U;
    (void)memset(record, 0xFF, sizeof(record));
    s_put_u32(record, CONFIG_MAGIC);
    s_put_u32(&record[4], CONFIG_VERSION);
    s_put_u32(&record[8], sequence);
    s_put_u32(&record[12], (uint32_t)format);
    s_put_u32(&record[CONFIG_CRC_OFFSET], s_crc(record, CONFIG_CRC_OFFSET));
    result = w25q128_erase_sector(p_context, address);
    if (result != W25Q128_OK) { return result; }
    result = w25q128_program(p_context, address, record, CONFIG_BODY_SIZE);
    if (result != W25Q128_OK) { return result; }
    result = w25q128_read(p_context, address, verify, sizeof(verify));
    if (result != W25Q128_OK) { return result; }
    if (memcmp(record, verify, sizeof(record)) != 0) { return W25Q128_ERROR_VERIFY; }
    s_put_u32(&record[CONFIG_COMMIT_OFFSET], CONFIG_COMMIT);
    result = w25q128_program(p_context, address + CONFIG_COMMIT_OFFSET,
                             &record[CONFIG_COMMIT_OFFSET], sizeof(uint32_t));
    if (result != W25Q128_OK) { return result; }
    result = w25q128_read(p_context, address, verify, sizeof(verify));
    if (result != W25Q128_OK) { return result; }
    if (memcmp(record, verify, sizeof(record)) != 0 || s_valid(verify) == 0U) {
        return W25Q128_ERROR_VERIFY;
    }
    p_state->format = format;
    p_state->sequence = sequence;
    p_state->active_slot = slot;
    p_state->valid = 1U;
    p_state->degraded = 0U;
    return W25Q128_OK;
}
