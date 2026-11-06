#include "main.h"
#include "stm32_adxl345.h"
#include "stm32_buttons.h"

/* The one HAL callback owner. Each adapter filters only its own EXTI lines. */
void HAL_GPIO_EXTI_Callback(uint16_t pin)
{
    stm32_adxl345_motion_exti_callback(pin);
    stm32_buttons_exti_callback(pin);
}
