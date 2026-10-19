#include "flash_w25q128.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Strict SPI NOR model: rejects cross-page program and commands while busy. */
static uint8_t s_memory[W25Q128_TOTAL_SIZE];
static struct {
    uint32_t tick;
    uint32_t busy_ms;
    uint32_t calls;
    uint32_t fail_at;
    uint32_t programs;
    uint32_t erases;
    uint32_t locks;
    uint32_t unlocks;
    uint8_t wel;
    uint8_t sr1;
    uint8_t sr2;
    uint8_t sr3;
    uint8_t asleep;
    uint8_t bad_id;
    uint8_t lock_fail;
    uint8_t locked;
    uint8_t no_wel;
    uint8_t force_busy;
    uint8_t stuck_on_write;
    uint8_t ignore_program;
    uint8_t ignore_erase;
} s_model;
static uint8_t s_scratch[W25Q128_SECTOR_SIZE];

static uint32_t s_address(const uint8_t *p_tx)
{
    return ((uint32_t)p_tx[1] << 16U) |
           ((uint32_t)p_tx[2] << 8U) | p_tx[3];
}

static int32_t s_transaction(void *p_handle, const uint8_t *p_tx,
                             uint32_t tx_len, uint8_t *p_rx, uint32_t rx_len)
{
    uint32_t address;
    uint32_t size;
    uint32_t i;
    (void)p_handle;
    assert(s_model.locked && p_tx != NULL && tx_len != 0U);
    s_model.calls++;
    if (s_model.calls == s_model.fail_at) { return -1; }
    if (p_tx[0] == 0xABU) { s_model.asleep = 0U; return 0; }
    assert(!s_model.asleep);
    switch (p_tx[0]) {
    case 0x05U:
        assert(tx_len == 1U && rx_len == 1U);
        p_rx[0] = (uint8_t)(s_model.sr1 | (s_model.wel ? 2U : 0U) |
                            ((s_model.busy_ms || s_model.force_busy) ? 1U : 0U));
        return 0;
    case 0x35U:
    case 0x15U:
        assert(tx_len == 1U && rx_len == 1U);
        p_rx[0] = p_tx[0] == 0x35U ? s_model.sr2 : s_model.sr3;
        return 0;
    default:
        assert(!s_model.busy_ms && !s_model.force_busy);
        break;
    }
    switch (p_tx[0]) {
    case 0x9FU:
        assert(tx_len == 1U && rx_len == 3U);
        p_rx[0] = s_model.bad_id ? 0U : 0xEFU;
        p_rx[1] = 0x40U; p_rx[2] = 0x18U;
        break;
    case 0x06U:
        assert(tx_len == 1U && rx_len == 0U);
        s_model.wel = s_model.no_wel ? 0U : 1U;
        break;
    case 0x03U:
        assert(tx_len == 4U && rx_len != 0U);
        address = s_address(p_tx);
        assert(rx_len <= W25Q128_TOTAL_SIZE - address);
        (void)memcpy(p_rx, &s_memory[address], rx_len);
        break;
    case 0x02U:
        assert(s_model.wel && tx_len > 4U && rx_len == 0U);
        address = s_address(p_tx);
        size = tx_len - 4U;
        assert(size <= W25Q128_PAGE_SIZE - address % W25Q128_PAGE_SIZE);
        if (!s_model.ignore_program) {
            for (i = 0U; i < size; i++) { s_memory[address + i] &= p_tx[4U + i]; }
        }
        s_model.programs++;
        s_model.wel = 0U;
        s_model.busy_ms = 3U;
        s_model.force_busy = s_model.stuck_on_write;
        break;
    case 0x20U:
    case 0x52U:
    case 0xD8U:
    case 0xC7U:
        assert(s_model.wel && rx_len == 0U);
        size = p_tx[0] == 0x20U ? W25Q128_SECTOR_SIZE :
               p_tx[0] == 0x52U ? W25Q128_BLOCK32_SIZE :
               p_tx[0] == 0xD8U ? W25Q128_BLOCK64_SIZE : W25Q128_TOTAL_SIZE;
        assert(tx_len == (p_tx[0] == 0xC7U ? 1U : 4U));
        address = p_tx[0] == 0xC7U ? 0U : s_address(p_tx);
        assert(address % size == 0U && size <= W25Q128_TOTAL_SIZE - address);
        if (!s_model.ignore_erase) { (void)memset(&s_memory[address], 0xFF, size); }
        s_model.erases++;
        s_model.wel = 0U;
        s_model.busy_ms = 5U;
        s_model.force_busy = s_model.stuck_on_write;
        break;
    default:
        assert(0 && "unexpected command");
    }
    return 0;
}

static void s_delay(void *p_handle, uint32_t delay_ms)
{
    (void)p_handle;
    assert(s_model.locked);
    s_model.tick += delay_ms;
    s_model.busy_ms = delay_ms >= s_model.busy_ms ? 0U : s_model.busy_ms - delay_ms;
}

static uint32_t s_now(void *p_handle) { (void)p_handle; return s_model.tick; }
static int32_t s_lock(void *p_handle)
{
    (void)p_handle;
    assert(!s_model.locked);
    if (s_model.lock_fail) { return -1; }
    s_model.locked = 1U;
    s_model.locks++;
    return 0;
}
static void s_unlock(void *p_handle)
{
    (void)p_handle;
    assert(s_model.locked);
    s_model.locked = 0U;
    s_model.unlocks++;
}
static void s_balanced(void)
{
    assert(!s_model.locked && s_model.locks == s_model.unlocks);
}
static void s_reset(void)
{
    s_balanced();
    (void)memset(&s_model, 0, sizeof(s_model));
    (void)memset(s_memory, 0xFF, sizeof(s_memory));
}

static void s_test_data(const w25q128_ctx_t *p_ctx)
{
    uint8_t data[700];
    uint8_t readback[700];
    uint8_t expected[2U * W25Q128_SECTOR_SIZE];
    uint32_t i;
    uint32_t erases;
    uint32_t programs;

    s_reset();
    s_model.asleep = 1U;
    assert(w25q128_init(p_ctx) == W25Q128_OK && s_model.tick >= 4U);
    for (i = 0U; i < sizeof(data); i++) { data[i] = (uint8_t)i; }
    assert(w25q128_program(p_ctx, 250U, data, sizeof(data)) == W25Q128_OK);
    assert(s_model.programs == 4U);
    assert(w25q128_read(p_ctx, 250U, readback, sizeof(readback)) == W25Q128_OK);
    assert(memcmp(data, readback, sizeof(data)) == 0);
    data[0] = 0xFFU;
    assert(w25q128_program(p_ctx, 250U, data, 1U) == W25Q128_ERROR_NEEDS_ERASE);
    assert(s_model.erases == 0U);
    assert(w25q128_program(p_ctx, W25Q128_TOTAL_SIZE - 1U, data, 1U) == W25Q128_OK);
    assert(w25q128_read(p_ctx, W25Q128_TOTAL_SIZE - 1U, readback, 1U) == W25Q128_OK);
    assert(readback[0] == data[0]);

    for (i = 0U; i < sizeof(expected); i++) { expected[i] = (uint8_t)(i * 7U); }
    (void)memcpy(s_memory, expected, sizeof(expected));
    (void)memcpy(&expected[4090U], data, 20U);
    assert(w25q128_update(p_ctx, 4090U, data, 20U, s_scratch,
                          sizeof(s_scratch)) == W25Q128_OK);
    assert(memcmp(s_memory, expected, sizeof(expected)) == 0);
    assert(s_memory[sizeof(expected)] == 0xFFU);
    erases = s_model.erases;
    programs = s_model.programs;
    assert(w25q128_update(p_ctx, 4090U, data, 20U, s_scratch,
                          sizeof(s_scratch)) == W25Q128_OK);
    assert(s_model.erases == erases && s_model.programs == programs);
    s_balanced();
    puts("PASS: wake, cross-page program, 0-to-1 rejection, end address, sector preservation");
}

static void s_test_arguments(const w25q128_ctx_t *p_ctx)
{
    w25q128_ctx_t invalid = *p_ctx;
    uint8_t byte = 0U;
    s_reset();
    assert(w25q128_init(NULL) == W25Q128_ERROR_ARGUMENT);
    invalid.unlock = NULL;
    assert(w25q128_init(&invalid) == W25Q128_ERROR_ARGUMENT);
    assert(w25q128_read(p_ctx, 0U, NULL, 1U) == W25Q128_ERROR_ARGUMENT);
    assert(w25q128_read(p_ctx, W25Q128_TOTAL_SIZE, NULL, 0U) == W25Q128_OK);
    assert(w25q128_program(p_ctx, W25Q128_TOTAL_SIZE, NULL, 0U) == W25Q128_OK);
    assert(w25q128_update(p_ctx, W25Q128_TOTAL_SIZE, NULL, 0U, NULL, 0U) == W25Q128_OK);
    assert(w25q128_read(p_ctx, UINT32_MAX, &byte, 2U) == W25Q128_ERROR_RANGE);
    assert(w25q128_program(p_ctx, W25Q128_TOTAL_SIZE - 1U, &byte, 2U) == W25Q128_ERROR_RANGE);
    assert(w25q128_read(p_ctx, 1U, &byte, UINT32_MAX) == W25Q128_ERROR_RANGE);
    assert(w25q128_erase_sector(p_ctx, 1U) == W25Q128_ERROR_RANGE);
    assert(w25q128_erase_block32(p_ctx, W25Q128_SECTOR_SIZE) == W25Q128_ERROR_RANGE);
    assert(w25q128_erase_block64(p_ctx, W25Q128_BLOCK32_SIZE) == W25Q128_ERROR_RANGE);
    assert(w25q128_erase_sector(p_ctx, W25Q128_TOTAL_SIZE) == W25Q128_ERROR_RANGE);
    assert(w25q128_update(p_ctx, 0U, &byte, 1U, s_scratch, sizeof(s_scratch) - 1U) ==
           W25Q128_ERROR_ARGUMENT);
    assert(w25q128_update(p_ctx, 0U, s_scratch + 1U, 1U, s_scratch, sizeof(s_scratch)) ==
           W25Q128_ERROR_ARGUMENT);
    assert(w25q128_update(p_ctx, 0U, s_scratch, 2U, s_scratch + 1U, sizeof(s_scratch)) ==
           W25Q128_ERROR_ARGUMENT);
    assert(s_model.calls == 0U);
    s_balanced();
    puts("PASS: arguments, overflow, alignment, scratch overlap, empty operations");
}

static void s_test_errors(const w25q128_ctx_t *p_ctx)
{
    uint8_t byte = 0U;
    uint32_t i;
    w25q128_registers_t registers;
    s_reset();
    s_model.bad_id = 1U;
    assert(w25q128_init(p_ctx) == W25Q128_ERROR_ID);
    s_model.bad_id = 0U;
    s_model.lock_fail = 1U;
    assert(w25q128_read(p_ctx, 0U, &byte, 1U) == W25Q128_ERROR_BUSY);
    s_model.lock_fail = 0U;
    s_model.sr2 = 0x80U;
    assert(w25q128_init(p_ctx) == W25Q128_ERROR_BUSY);
    assert(w25q128_program(p_ctx, 0U, &byte, 1U) == W25Q128_ERROR_BUSY);
    for (i = 0U; i < 3U; i++) {
        s_model.sr1 = i == 0U ? 4U : 0U;
        s_model.sr2 = i == 1U ? 0x40U : 0U;
        s_model.sr3 = i == 2U ? 4U : 0U;
        assert(w25q128_read_registers(p_ctx, &registers) == W25Q128_OK);
        assert(registers.sr1 == s_model.sr1 && registers.sr2 == s_model.sr2 &&
               registers.sr3 == s_model.sr3);
        assert(w25q128_program(p_ctx, 0U, &byte, 1U) == W25Q128_ERROR_PROTECTED);
        assert(w25q128_erase_sector(p_ctx, 0U) == W25Q128_ERROR_PROTECTED);
    }
    assert(s_model.programs == 0U && s_model.erases == 0U);
    s_reset();
    s_model.no_wel = 1U;
    assert(w25q128_program(p_ctx, 0U, &byte, 1U) == W25Q128_ERROR_TIMEOUT);
    assert(s_model.tick == 10U && s_model.programs == 0U);
    s_reset();
    s_model.ignore_program = 1U;
    assert(w25q128_program(p_ctx, 0U, &byte, 1U) == W25Q128_ERROR_VERIFY);
    s_reset();
    s_memory[0] = 0U;
    s_model.ignore_erase = 1U;
    assert(w25q128_erase_sector(p_ctx, 0U) == W25Q128_ERROR_VERIFY);
    s_balanced();
    puts("PASS: ID, mutex, suspended operation, protection, WEL timeout, readback failures");
}

static void s_test_erase_and_timeouts(const w25q128_ctx_t *p_ctx)
{
    w25q128_status_t (*const erase[])(const w25q128_ctx_t *, uint32_t) = {
        w25q128_erase_sector, w25q128_erase_block32, w25q128_erase_block64
    };
    const uint32_t sizes[] = {4096U, 32768U, 65536U};
    const uint32_t timeouts[] = {500U, 2000U, 3000U};
    uint32_t i;
    uint32_t start;
    uint8_t byte = 0U;
    for (i = 0U; i < 3U; i++) {
        s_reset();
        (void)memset(s_memory, 0, 2U * sizes[i]);
        assert(erase[i](p_ctx, sizes[i]) == W25Q128_OK);
        assert(s_memory[sizes[i] - 1U] == 0U && s_memory[sizes[i]] == 0xFFU &&
               s_memory[2U * sizes[i] - 1U] == 0xFFU);
        s_model.stuck_on_write = 1U;
        start = s_model.tick;
        assert(erase[i](p_ctx, sizes[i]) == W25Q128_ERROR_TIMEOUT);
        assert(s_model.tick - start == timeouts[i]);
    }
    s_reset();
    (void)memset(s_memory, 0, sizeof(s_memory));
    assert(w25q128_erase_chip(p_ctx) == W25Q128_OK);
    assert(s_memory[0] == 0xFFU && s_memory[W25Q128_TOTAL_SIZE - 1U] == 0xFFU);
    s_model.stuck_on_write = 1U;
    start = s_model.tick;
    assert(w25q128_erase_chip(p_ctx) == W25Q128_ERROR_TIMEOUT);
    assert(s_model.tick - start == 205000U);
    s_reset();
    s_model.tick = UINT32_MAX - 10U;
    s_model.stuck_on_write = 1U;
    start = s_model.tick;
    assert(w25q128_program(p_ctx, 0U, &byte, 1U) == W25Q128_ERROR_TIMEOUT);
    assert(s_model.tick - start == 100U);
    s_reset();
    s_model.force_busy = 1U;
    assert(w25q128_read(p_ctx, 0U, &byte, 1U) == W25Q128_ERROR_TIMEOUT);
    assert(s_model.tick == 205000U);
    s_balanced();
    puts("PASS: all erase sizes, bounded WIP waits, uint32 tick wrap");
}

static w25q128_status_t s_scenario(const w25q128_ctx_t *p_ctx, uint32_t scenario)
{
    static const uint8_t data[] = {0x12U, 0x34U, 0x56U};
    switch (scenario) {
    case 0U: return w25q128_init(p_ctx);
    case 1U: return w25q128_program(p_ctx, 255U, data, sizeof(data));
    case 2U: return w25q128_erase_sector(p_ctx, 0U);
    default: return w25q128_update(p_ctx, 4095U, data, sizeof(data),
                                  s_scratch, sizeof(s_scratch));
    }
}

static void s_test_io_failures(const w25q128_ctx_t *p_ctx)
{
    uint32_t scenario;
    uint32_t calls;
    uint32_t fault;
    uint32_t total = 0U;
    for (scenario = 0U; scenario < 4U; scenario++) {
        s_reset();
        assert(s_scenario(p_ctx, scenario) == W25Q128_OK);
        calls = s_model.calls;
        for (fault = 1U; fault <= calls; fault++) {
            s_reset();
            s_model.fail_at = fault;
            assert(s_scenario(p_ctx, scenario) == W25Q128_ERROR_IO);
            assert(s_model.calls == fault);
            s_balanced();
            total++;
        }
    }
    printf("PASS: %u injected transaction failures, immediate exit and balanced mutex\n",
           (unsigned int)total);
}

int main(void)
{
    const w25q128_ctx_t context = {
        s_transaction, s_delay, s_now, s_lock, s_unlock, NULL
    };
    s_test_data(&context);
    s_test_arguments(&context);
    s_test_errors(&context);
    s_test_erase_and_timeouts(&context);
    s_test_io_failures(&context);
    puts("w25q128 host tests passed");
    return 0;
}
