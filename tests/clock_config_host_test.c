#include "clock_config.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t s_memory[2U * W25Q128_SECTOR_SIZE];
static uint8_t s_baseline[sizeof(s_memory)];
static uint32_t s_calls;
static uint32_t s_fail_at;
static uint32_t s_partial;
static uint8_t s_after;
static uint8_t s_off;
static w25q128_ctx_t s_context;

static uint32_t s_index(uint32_t address, uint32_t length)
{
    assert(address >= CLOCK_CONFIG_SECTOR_A);
    assert(address + length <= W25Q128_TOTAL_SIZE);
    return address - CLOCK_CONFIG_SECTOR_A;
}

static uint8_t s_fail(void)
{
    s_calls++;
    if (s_off != 0U) { return 1U; }
    if (s_calls == s_fail_at) { s_off = 1U; return 1U; }
    return 0U;
}

w25q128_status_t w25q128_read(const w25q128_ctx_t *p_context, uint32_t address,
                              uint8_t *p_data, uint32_t length)
{
    uint32_t index = s_index(address, length);
    (void)p_context;
    if (s_fail() != 0U) { return W25Q128_ERROR_IO; }
    (void)memcpy(p_data, &s_memory[index], length);
    return W25Q128_OK;
}

w25q128_status_t w25q128_erase_sector(const w25q128_ctx_t *p_context, uint32_t address)
{
    uint32_t index = s_index(address, W25Q128_SECTOR_SIZE);
    uint8_t fail = s_fail();
    (void)p_context;
    assert(address % W25Q128_SECTOR_SIZE == 0U);
    if (fail != 0U) {
        if (s_after != 0U) { (void)memset(&s_memory[index], 0xFF, s_partial); }
        return W25Q128_ERROR_IO;
    }
    (void)memset(&s_memory[index], 0xFF, W25Q128_SECTOR_SIZE);
    return W25Q128_OK;
}

w25q128_status_t w25q128_program(const w25q128_ctx_t *p_context, uint32_t address,
                                 const uint8_t *p_data, uint32_t length)
{
    uint32_t index = s_index(address, length);
    uint32_t count;
    uint32_t i;
    uint8_t fail = s_fail();
    (void)p_context;
    count = fail == 0U ? length : (s_after != 0U ? s_partial : 0U);
    assert(count <= length);
    for (i = 0U; i < count; i++) {
        assert((s_memory[index + i] & p_data[i]) == p_data[i]);
        s_memory[index + i] &= p_data[i];
    }
    return fail != 0U ? W25Q128_ERROR_IO : W25Q128_OK;
}

static void s_reset_failure(void)
{
    s_calls = 0U; s_fail_at = 0U; s_partial = 0U; s_after = 0U; s_off = 0U;
}

static void s_test_load_and_save(void)
{
    clock_config_state_t state;
    uint32_t calls;
    s_reset_failure();
    (void)memset(s_memory, 0xFF, sizeof(s_memory));
    assert(clock_config_load(&s_context, &state) == W25Q128_OK);
    assert(state.format == CLOCK_FORMAT_24 && state.valid == 0U);
    assert(clock_config_save(&s_context, &state, CLOCK_FORMAT_12) == W25Q128_OK);
    assert(state.format == CLOCK_FORMAT_12 && state.sequence == 1U && state.active_slot == 0U);
    calls = s_calls;
    assert(clock_config_save(&s_context, &state, CLOCK_FORMAT_12) == W25Q128_OK);
    assert(s_calls == calls);
    assert(clock_config_save(&s_context, &state, CLOCK_FORMAT_24) == W25Q128_OK);
    assert(state.sequence == 2U && state.active_slot == 1U);
    (void)memcpy(s_baseline, s_memory, sizeof(s_memory));
    assert(clock_config_load(&s_context, &state) == W25Q128_OK);
    assert(state.format == CLOCK_FORMAT_24 && state.sequence == 2U);
    s_memory[W25Q128_SECTOR_SIZE + 16U] ^= 1U;
    assert(clock_config_load(&s_context, &state) == W25Q128_OK);
    assert(state.format == CLOCK_FORMAT_12 && state.degraded != 0U);
    s_memory[16] ^= 1U;
    assert(clock_config_load(&s_context, &state) == CLOCK_CONFIG_ERROR_CORRUPT);
    assert(state.format == CLOCK_FORMAT_24 && state.valid == 0U);
    assert(clock_config_save(&s_context, &state, CLOCK_FORMAT_24) == W25Q128_OK);
    assert(clock_config_load(&s_context, &state) == W25Q128_OK && state.valid != 0U);
    assert(clock_config_save(&s_context, &state, (clock_format_t)13) == W25Q128_ERROR_ARGUMENT);
    assert(clock_config_load(NULL, &state) == W25Q128_ERROR_ARGUMENT);
}

static void s_test_wrap(void)
{
    clock_config_state_t state;
    s_reset_failure();
    (void)memset(s_memory, 0xFF, sizeof(s_memory));
    assert(clock_config_load(&s_context, &state) == W25Q128_OK);
    state.sequence = 0xFFFFFFFEUL;
    assert(clock_config_save(&s_context, &state, CLOCK_FORMAT_12) == W25Q128_OK);
    assert(state.sequence == 0xFFFFFFFFUL);
    assert(clock_config_save(&s_context, &state, CLOCK_FORMAT_24) == W25Q128_OK);
    assert(state.sequence == 0U);
    assert(clock_config_load(&s_context, &state) == W25Q128_OK);
    assert(state.sequence == 0U && state.format == CLOCK_FORMAT_24);
    assert(clock_config_save(&s_context, &state, CLOCK_FORMAT_12) == W25Q128_OK);
    assert(clock_config_load(&s_context, &state) == W25Q128_OK);
    assert(state.sequence == 1U && state.format == CLOCK_FORMAT_12);
}

static void s_cut(uint32_t fail_at, uint8_t after, uint32_t partial)
{
    clock_config_state_t before;
    clock_config_state_t state;
    s_reset_failure();
    (void)memcpy(s_memory, s_baseline, sizeof(s_memory));
    assert(clock_config_load(&s_context, &state) == W25Q128_OK);
    before = state;
    s_calls = 0U; s_fail_at = fail_at; s_after = after; s_partial = partial;
    assert(clock_config_save(&s_context, &state, CLOCK_FORMAT_12) == W25Q128_ERROR_IO);
    assert(memcmp(&state, &before, sizeof(state)) == 0);
    /* The previous committed sector must remain byte-for-byte intact. */
    assert(memcmp(&s_memory[W25Q128_SECTOR_SIZE],
                  &s_baseline[W25Q128_SECTOR_SIZE], W25Q128_SECTOR_SIZE) == 0);
    s_reset_failure();
    assert(clock_config_load(&s_context, &state) == W25Q128_OK);
    assert((state.sequence == 2U && state.format == CLOCK_FORMAT_24) ||
           (state.sequence == 3U && state.format == CLOCK_FORMAT_12));
    assert(state.valid != 0U);
    /* Explicit retry first reloads, then may safely commit either direction. */
    assert(clock_config_save(&s_context, &state, CLOCK_FORMAT_12) == W25Q128_OK);
    assert(clock_config_load(&s_context, &state) == W25Q128_OK);
    assert(state.format == CLOCK_FORMAT_12);
}

static void s_test_power_loss(void)
{
    uint32_t boundary;
    uint32_t count;
    for (boundary = 1U; boundary <= 5U; boundary++) { s_cut(boundary, 0U, 0U); }
    for (count = 0U; count <= W25Q128_SECTOR_SIZE; count++) { s_cut(1U, 1U, count); }
    for (count = 0U; count <= 20U; count++) { s_cut(2U, 1U, count); }
    for (count = 0U; count <= 4U; count++) { s_cut(4U, 1U, count); }
}

int main(void)
{
    s_test_load_and_save();
    s_test_wrap();
    s_test_power_loss();
    (void)puts("clock_config: load, CRC, sequence wrap, recovery and 4128 power-loss cuts passed");
    return 0;
}
