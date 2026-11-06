#ifndef BUTTON_FILTER_H
#define BUTTON_FILTER_H
#include <stdbool.h>
#include <stdint.h>

#define BUTTON_FILTER_COUNT 4U
#define BUTTON_FILTER_MASK ((1UL << BUTTON_FILTER_COUNT) - 1UL)
#define BUTTON_FILTER_DEBOUNCE_MS 30U

/* Caller-owned state. Time is supplied by the caller, in wrapping uint32 ms. */
typedef struct {
    uint32_t changed_ms[BUTTON_FILTER_COUNT];
    uint32_t candidate;
    uint32_t stable;
} button_filter_t;
typedef struct {
    uint32_t pressed_mask;
    bool needs_sampling;
} button_filter_result_t;

void button_filter_init(button_filter_t *p_filter);
button_filter_result_t button_filter_update(button_filter_t *p_filter,
                                            uint32_t raw_mask, uint32_t now_ms);
#endif
