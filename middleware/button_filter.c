#include "button_filter.h"
#include <string.h>

void button_filter_init(button_filter_t *p_filter)
{
    if (p_filter != NULL) { memset(p_filter, 0, sizeof(*p_filter)); }
}

button_filter_result_t button_filter_update(button_filter_t *p_filter,
                                            uint32_t raw_mask, uint32_t now_ms)
{
    button_filter_result_t result = {0U, false};
    uint32_t bit;
    uint32_t i;
    if (p_filter == NULL) { return result; }
    raw_mask &= BUTTON_FILTER_MASK;
    for (i = 0U; i < BUTTON_FILTER_COUNT; i++) {
        bit = 1UL << i;
        if (((raw_mask ^ p_filter->candidate) & bit) != 0U) {
            p_filter->candidate ^= bit;
            p_filter->changed_ms[i] = now_ms;
        }
        if (((p_filter->candidate ^ p_filter->stable) & bit) != 0U &&
            (uint32_t)(now_ms - p_filter->changed_ms[i]) >= BUTTON_FILTER_DEBOUNCE_MS) {
            p_filter->stable ^= bit;
            result.pressed_mask |= p_filter->stable & bit;
        }
    }
    result.needs_sampling = (raw_mask | p_filter->candidate | p_filter->stable) != 0U;
    return result;
}
