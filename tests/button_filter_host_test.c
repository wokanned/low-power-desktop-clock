#include "button_filter.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    button_filter_t filter;
    button_filter_result_t result;
    uint32_t i;
    button_filter_init(NULL);
    assert(button_filter_update(NULL, 15U, 0U).pressed_mask == 0U);
    button_filter_init(&filter);
    assert(!button_filter_update(&filter, 0U, 0U).needs_sampling);
    assert(button_filter_update(&filter, 1U, 10U).needs_sampling);
    assert(button_filter_update(&filter, 0U, 20U).pressed_mask == 0U);
    assert(button_filter_update(&filter, 1U, 25U).pressed_mask == 0U);
    assert(button_filter_update(&filter, 1U, 54U).pressed_mask == 0U);
    assert(button_filter_update(&filter, 1U, 55U).pressed_mask == 1U);
    for (i = 56U; i < 400U; i++) {
        result = button_filter_update(&filter, 1U, i);
        assert(result.pressed_mask == 0U && result.needs_sampling);
    }
    /* Release bounce cannot rearm a key before a stable release. */
    assert(button_filter_update(&filter, 0U, 400U).needs_sampling);
    assert(button_filter_update(&filter, 1U, 410U).pressed_mask == 0U);
    assert(button_filter_update(&filter, 0U, 420U).needs_sampling);
    assert(button_filter_update(&filter, 0U, 449U).needs_sampling);
    assert(!button_filter_update(&filter, 0U, 450U).needs_sampling);
    assert(button_filter_update(&filter, 1U, 451U).pressed_mask == 0U);
    assert(button_filter_update(&filter, 1U, 481U).pressed_mask == 1U);
    /* Keys debounce independently; an extra bit is not a fifth key. */
    button_filter_init(&filter);
    assert(button_filter_update(&filter, 0x11U, 0U).pressed_mask == 0U);
    assert(button_filter_update(&filter, 3U, 10U).pressed_mask == 0U);
    assert(button_filter_update(&filter, 3U, 30U).pressed_mask == 1U);
    assert(button_filter_update(&filter, 3U, 40U).pressed_mask == 2U);
    assert(button_filter_update(&filter, 0U, 50U).needs_sampling);
    assert(!button_filter_update(&filter, 0U, 80U).needs_sampling);
    assert(!button_filter_update(&filter, 0x10U, 90U).needs_sampling);
    button_filter_init(&filter);
    assert(button_filter_update(&filter, 15U, UINT32_MAX - 19U).pressed_mask == 0U);
    assert(button_filter_update(&filter, 15U, 9U).pressed_mask == 0U);
    assert(button_filter_update(&filter, 15U, 10U).pressed_mask == 15U);
    assert(button_filter_update(&filter, 0U, 20U).needs_sampling);
    assert(!button_filter_update(&filter, 0U, 50U).needs_sampling);
    puts("PASS: debounce boundaries, bounce, held keys, stable release, independent keys, invalid bits and time wrap");
    return 0;
}
