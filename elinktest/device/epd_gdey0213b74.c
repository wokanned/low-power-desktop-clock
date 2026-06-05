#include "epd_gdey0213b74.h"

/*
 * Device layer for the GDEY0213B74-FT11 panel.
 * This file owns only controller commands, memory layout, and timing rules.
 * It does not depend on HAL, GPIO names, or any board-specific resource.
 */

#define EPD_GDEY0213B74_CMD_SW_RESET            0x12U
#define EPD_GDEY0213B74_CMD_DRIVER_OUTPUT       0x01U
#define EPD_GDEY0213B74_CMD_DATA_ENTRY_MODE     0x11U
#define EPD_GDEY0213B74_CMD_RAM_X_WINDOW        0x44U
#define EPD_GDEY0213B74_CMD_RAM_Y_WINDOW        0x45U
#define EPD_GDEY0213B74_CMD_BORDER_WAVEFORM     0x3CU
#define EPD_GDEY0213B74_CMD_UPDATE_CONTROL      0x21U
#define EPD_GDEY0213B74_CMD_TEMP_SENSOR         0x18U
#define EPD_GDEY0213B74_CMD_BOOSTER_SOFT_START  0x0CU
#define EPD_GDEY0213B74_CMD_RAM_X_CURSOR        0x4EU
#define EPD_GDEY0213B74_CMD_RAM_Y_CURSOR        0x4FU
#define EPD_GDEY0213B74_CMD_WRITE_RAM           0x24U
#define EPD_GDEY0213B74_CMD_DISPLAY_UPDATE_2    0x22U
#define EPD_GDEY0213B74_CMD_MASTER_ACTIVATE     0x20U
#define EPD_GDEY0213B74_CMD_DEEP_SLEEP          0x10U
#define EPD_GDEY0213B74_POWER_ON_DELAY_MS       10U

/* 0x22 Display Update Control 2 parameters (see datasheet section 7). */
#define EPD_GDEY0213B74_DISPLAY_UPDATE_LUT_OTP   0xB1U
#define EPD_GDEY0213B74_DISPLAY_UPDATE_FULL      0xF7U

/* Booster Soft Start POR values (0x0C data bytes A/B/C/D, see datasheet §7). */
#define EPD_GDEY0213B74_SOFT_START_PHASE1_POR    0x8BU
#define EPD_GDEY0213B74_SOFT_START_PHASE2_POR    0x9CU
#define EPD_GDEY0213B74_SOFT_START_PHASE3_POR    0x96U
#define EPD_GDEY0213B74_SOFT_START_DURATION_POR  0x0FU

/*
 * Every public device API starts from the same sanity check so the panel state
 * machine never runs with a missing callback or an undersized frame buffer.
 */
static int32_t epd_gdey0213b74_validate_display(const epd_gdey0213b74_t *display)
{
    if ((display == 0) ||
        (display->framebuffer == 0) ||
        (display->framebuffer_size < EPD_GDEY0213B74_FRAMEBUFFER_SIZE_BYTES) ||
        (display->io.spi_write == 0) ||
        (display->io.read_busy == 0) ||
        (display->io.set_cs == 0) ||
        (display->io.set_dc == 0) ||
        (display->io.set_reset == 0) ||
        (display->io.delay_ms == 0) ||
        (display->io.get_tick_ms == 0))
    {
        return EPD_GDEY0213B74_ERR_PARAM;
    }

    return EPD_GDEY0213B74_OK;
}

/*
 * Send one controller command followed by an optional data payload.
 * The panel distinguishes command and data by DC, so each transaction toggles
 * DC first and then keeps CS around the exact byte stream being transferred.
 */
static int32_t epd_gdey0213b74_send_sequence(epd_gdey0213b74_t *display,
                                             uint8_t command,
                                             const uint8_t *data,
                                             uint16_t length)
{
    uint16_t data_index;
    int32_t status;

    status = epd_gdey0213b74_validate_display(display);
    if (status != EPD_GDEY0213B74_OK)
    {
        return status;
    }

    display->io.set_dc(display->io.handle, 0U);
    display->io.set_cs(display->io.handle, 0U);
    status = display->io.spi_write(display->io.handle, &command, 1U);
    display->io.set_cs(display->io.handle, 1U);
    if (status != 0)
    {
        return EPD_GDEY0213B74_ERR_IO;
    }

    if ((data != 0) && (length > 0U))
    {
        display->io.set_dc(display->io.handle, 1U);
        for (data_index = 0U; data_index < length; data_index++)
        {
            display->io.set_cs(display->io.handle, 0U);
            status = display->io.spi_write(display->io.handle, &data[data_index], 1U);
            display->io.set_cs(display->io.handle, 1U);
            if (status != 0)
            {
                return EPD_GDEY0213B74_ERR_IO;
            }
        }
    }

    return EPD_GDEY0213B74_OK;
}

/*
 * BUSY is high while the controller is resetting, loading waveform data, or
 * driving the glass. The timeout keeps firmware from hanging forever on a bad
 * panel, a bad power rail, or a broken BUSY connection.
 */
static int32_t epd_gdey0213b74_wait_until_idle(epd_gdey0213b74_t *display, uint32_t timeout_ms)
{
    uint8_t busy_level;
    uint32_t start_tick;
    int32_t status;

    status = epd_gdey0213b74_validate_display(display);
    if (status != EPD_GDEY0213B74_OK)
    {
        return status;
    }

    start_tick = display->io.get_tick_ms(display->io.handle);
    while (1)
    {
        status = display->io.read_busy(display->io.handle, &busy_level);
        if (status != 0)
        {
            return EPD_GDEY0213B74_ERR_IO;
        }

        if (busy_level == 0U)
        {
            return EPD_GDEY0213B74_OK;
        }

        if ((display->io.get_tick_ms(display->io.handle) - start_tick) > timeout_ms)
        {
            return EPD_GDEY0213B74_ERR_TIMEOUT;
        }

        display->io.delay_ms(display->io.handle, 1U);
    }
}

/*
 * Hardware reset follows the datasheet minimum timing: hold reset low for at
 * least 10 ms, release it, and then give the controller time to restart.
 */
static void epd_gdey0213b74_reset_panel(epd_gdey0213b74_t *display)
{
    display->io.set_cs(display->io.handle, 1U);
    display->io.set_dc(display->io.handle, 1U);
    display->io.set_reset(display->io.handle, 0U);
    display->io.delay_ms(display->io.handle, 10U);
    display->io.set_reset(display->io.handle, 1U);
    display->io.delay_ms(display->io.handle, 10U);
}

/*
 * Bind the caller-owned frame buffer and the platform callback table into one
 * device object. The driver stays stateless beyond this object so later ports
 * can reuse the same device layer unchanged.
 */
int32_t epd_gdey0213b74_bind(epd_gdey0213b74_t *display,
                             const epd_gdey0213b74_io_t *io,
                             uint8_t *framebuffer,
                             uint32_t framebuffer_size)
{
    if ((display == 0) || (io == 0) || (framebuffer == 0) ||
        (framebuffer_size < EPD_GDEY0213B74_FRAMEBUFFER_SIZE_BYTES))
    {
        return EPD_GDEY0213B74_ERR_PARAM;
    }

    display->io = *io;
    display->framebuffer = framebuffer;
    display->framebuffer_size = framebuffer_size;

    return epd_gdey0213b74_validate_display(display);
}

/*
 * Run the power-on sequence described by the panel datasheet section 14.1.
 * Steps: power wait, HW+SW reset, geometry/window registers, control registers,
 * temperature source, OTP LUT load, then preset RAM cursors.
 */
int32_t epd_gdey0213b74_panel_init(epd_gdey0213b74_t *display)
{
    uint8_t command_data[4];
    int32_t status;

    status = epd_gdey0213b74_validate_display(display);
    if (status != EPD_GDEY0213B74_OK)
    {
        return status;
    }

    /* Give the panel rails time to settle before the reset pulse starts. */
    display->io.delay_ms(display->io.handle, EPD_GDEY0213B74_POWER_ON_DELAY_MS);
    epd_gdey0213b74_reset_panel(display);

    /* Wait for the controller to leave the previous state, then issue SW reset. */
    status = epd_gdey0213b74_wait_until_idle(display, EPD_GDEY0213B74_BUSY_TIMEOUT_MS);
    if (status != EPD_GDEY0213B74_OK)
    {
        return status;
    }

    status = epd_gdey0213b74_send_sequence(display, EPD_GDEY0213B74_CMD_SW_RESET, 0, 0U);
    if (status != EPD_GDEY0213B74_OK)
    {
        return status;
    }

    status = epd_gdey0213b74_wait_until_idle(display, EPD_GDEY0213B74_BUSY_TIMEOUT_MS);
    if (status != EPD_GDEY0213B74_OK)
    {
        return status;
    }

    /* Program the controller RAM geometry used by this 122 x 250 panel. */
    command_data[0] = (uint8_t)((EPD_GDEY0213B74_RAM_HEIGHT_PIXELS - 1U) & 0xFFU);
    command_data[1] = (uint8_t)(((EPD_GDEY0213B74_RAM_HEIGHT_PIXELS - 1U) >> 8U) & 0xFFU);
    command_data[2] = 0x00U;
    status = epd_gdey0213b74_send_sequence(display, EPD_GDEY0213B74_CMD_DRIVER_OUTPUT, command_data, 3U);
    if (status != EPD_GDEY0213B74_OK)
    {
        return status;
    }

    /*
     * The vendor reference uses data-entry mode 0x01 together with the rotated
     * memory mapping implemented in epd_gdey0213b74_set_pixel().
     */
    command_data[0] = 0x01U;
    status = epd_gdey0213b74_send_sequence(display, EPD_GDEY0213B74_CMD_DATA_ENTRY_MODE, command_data, 1U);
    if (status != EPD_GDEY0213B74_OK)
    {
        return status;
    }

    command_data[0] = 0x00U;
    command_data[1] = (uint8_t)(EPD_GDEY0213B74_RAM_WIDTH_BYTES - 1U);
    status = epd_gdey0213b74_send_sequence(display, EPD_GDEY0213B74_CMD_RAM_X_WINDOW, command_data, 2U);
    if (status != EPD_GDEY0213B74_OK)
    {
        return status;
    }

    command_data[0] = (uint8_t)((EPD_GDEY0213B74_RAM_HEIGHT_PIXELS - 1U) & 0xFFU);
    command_data[1] = (uint8_t)(((EPD_GDEY0213B74_RAM_HEIGHT_PIXELS - 1U) >> 8U) & 0xFFU);
    command_data[2] = 0x00U;
    command_data[3] = 0x00U;
    status = epd_gdey0213b74_send_sequence(display, EPD_GDEY0213B74_CMD_RAM_Y_WINDOW, command_data, 4U);
    if (status != EPD_GDEY0213B74_OK)
    {
        return status;
    }

    command_data[0] = 0x05U;
    status = epd_gdey0213b74_send_sequence(display, EPD_GDEY0213B74_CMD_BORDER_WAVEFORM, command_data, 1U);
    if (status != EPD_GDEY0213B74_OK)
    {
        return status;
    }

    command_data[0] = 0x00U;
    command_data[1] = 0x80U;
    status = epd_gdey0213b74_send_sequence(display, EPD_GDEY0213B74_CMD_UPDATE_CONTROL, command_data, 2U);
    if (status != EPD_GDEY0213B74_OK)
    {
        return status;
    }

    /* Select the internal temperature source. */
    command_data[0] = 0x80U;
    status = epd_gdey0213b74_send_sequence(display, EPD_GDEY0213B74_CMD_TEMP_SENSOR, command_data, 1U);
    if (status != EPD_GDEY0213B74_OK)
    {
        return status;
    }

    /* Load waveform LUT from OTP: 0x22=0xB1 (enable clock, sense temp, load LUT Mode 1,
     * disable clock), then 0x20 to activate, then wait BUSY.  Matches datasheet step 4. */
    command_data[0] = EPD_GDEY0213B74_DISPLAY_UPDATE_LUT_OTP;
    status = epd_gdey0213b74_send_sequence(display, EPD_GDEY0213B74_CMD_DISPLAY_UPDATE_2, command_data, 1U);
    if (status != EPD_GDEY0213B74_OK)
    {
        return status;
    }

    status = epd_gdey0213b74_send_sequence(display, EPD_GDEY0213B74_CMD_MASTER_ACTIVATE, 0, 0U);
    if (status != EPD_GDEY0213B74_OK)
    {
        return status;
    }

    status = epd_gdey0213b74_wait_until_idle(display, EPD_GDEY0213B74_BUSY_TIMEOUT_MS);
    if (status != EPD_GDEY0213B74_OK)
    {
        return status;
    }

    /* The first image write starts from X = 0 and from the top of the rotated RAM. */
    command_data[0] = 0x00U;
    status = epd_gdey0213b74_send_sequence(display, EPD_GDEY0213B74_CMD_RAM_X_CURSOR, command_data, 1U);
    if (status != EPD_GDEY0213B74_OK)
    {
        return status;
    }

    command_data[0] = (uint8_t)((EPD_GDEY0213B74_RAM_HEIGHT_PIXELS - 1U) & 0xFFU);
    command_data[1] = (uint8_t)(((EPD_GDEY0213B74_RAM_HEIGHT_PIXELS - 1U) >> 8U) & 0xFFU);
    status = epd_gdey0213b74_send_sequence(display, EPD_GDEY0213B74_CMD_RAM_Y_CURSOR, command_data, 2U);
    if (status != EPD_GDEY0213B74_OK)
    {
        return status;
    }

    return epd_gdey0213b74_wait_until_idle(display, EPD_GDEY0213B74_BUSY_TIMEOUT_MS);
}

/*
 * Write the entire caller-owned frame buffer and trigger one full refresh.
 * The RAM cursors are always reloaded here so every refresh starts from a known
 * address even if future code adds partial updates or multiple full updates.
 */
int32_t epd_gdey0213b74_display_frame(epd_gdey0213b74_t *display)
{
    uint8_t command_data[4];
    int32_t status;

    status = epd_gdey0213b74_validate_display(display);
    if (status != EPD_GDEY0213B74_OK)
    {
        return status;
    }

    /* Match the datasheet image-write sequence: 0x4E, 0x4F, then 0x24 payload. */
    command_data[0] = 0x00U;
    status = epd_gdey0213b74_send_sequence(display, EPD_GDEY0213B74_CMD_RAM_X_CURSOR, command_data, 1U);
    if (status != EPD_GDEY0213B74_OK)
    {
        return status;
    }

    command_data[0] = (uint8_t)((EPD_GDEY0213B74_RAM_HEIGHT_PIXELS - 1U) & 0xFFU);
    command_data[1] = (uint8_t)(((EPD_GDEY0213B74_RAM_HEIGHT_PIXELS - 1U) >> 8U) & 0xFFU);
    status = epd_gdey0213b74_send_sequence(display, EPD_GDEY0213B74_CMD_RAM_Y_CURSOR, command_data, 2U);
    if (status != EPD_GDEY0213B74_OK)
    {
        return status;
    }

    status = epd_gdey0213b74_send_sequence(display,
                                           EPD_GDEY0213B74_CMD_WRITE_RAM,
                                           display->framebuffer,
                                           (uint16_t)EPD_GDEY0213B74_FRAMEBUFFER_SIZE_BYTES);
    if (status != EPD_GDEY0213B74_OK)
    {
        return status;
    }

    /*
     * Set booster soft start parameters before driving the panel.
     * Datasheet section 7, command 0x0C: 4 data bytes control the three-phase
     * soft start current and duration. POR values are used; future tuning
     * can adjust driving strength or phase duration here.
     */
    command_data[0] = EPD_GDEY0213B74_SOFT_START_PHASE1_POR;
    command_data[1] = EPD_GDEY0213B74_SOFT_START_PHASE2_POR;
    command_data[2] = EPD_GDEY0213B74_SOFT_START_PHASE3_POR;
    command_data[3] = EPD_GDEY0213B74_SOFT_START_DURATION_POR;
    status = epd_gdey0213b74_send_sequence(display,
                                           EPD_GDEY0213B74_CMD_BOOSTER_SOFT_START,
                                           command_data,
                                           4U);
    if (status != EPD_GDEY0213B74_OK)
    {
        return status;
    }

    command_data[0] = EPD_GDEY0213B74_DISPLAY_UPDATE_FULL;
    status = epd_gdey0213b74_send_sequence(display, EPD_GDEY0213B74_CMD_DISPLAY_UPDATE_2, command_data, 1U);
    if (status != EPD_GDEY0213B74_OK)
    {
        return status;
    }

    status = epd_gdey0213b74_send_sequence(display, EPD_GDEY0213B74_CMD_MASTER_ACTIVATE, 0, 0U);
    if (status != EPD_GDEY0213B74_OK)
    {
        return status;
    }

    return epd_gdey0213b74_wait_until_idle(display, EPD_GDEY0213B74_BUSY_TIMEOUT_MS);
}

/*
 * Enter deep sleep after the full refresh is complete.
 * E-paper keeps the final image without keeping the analog driving blocks on.
 */
int32_t epd_gdey0213b74_enter_deep_sleep(epd_gdey0213b74_t *display)
{
    uint8_t command_data[1];
    int32_t status;

    status = epd_gdey0213b74_validate_display(display);
    if (status != EPD_GDEY0213B74_OK)
    {
        return status;
    }

    command_data[0] = 0x01U;
    status = epd_gdey0213b74_send_sequence(display, EPD_GDEY0213B74_CMD_DEEP_SLEEP, command_data, 1U);
    if (status != EPD_GDEY0213B74_OK)
    {
        return status;
    }

    display->io.delay_ms(display->io.handle, 100U);
    return EPD_GDEY0213B74_OK;
}

/*
 * Fill the frame buffer with one byte pattern.
 * 0xFF means white pixels, 0x00 means black pixels for this controller.
 */
void epd_gdey0213b74_clear_framebuffer(epd_gdey0213b74_t *display, uint8_t color)
{
    uint32_t index;

    if (epd_gdey0213b74_validate_display(display) != EPD_GDEY0213B74_OK)
    {
        return;
    }

    for (index = 0U; index < EPD_GDEY0213B74_FRAMEBUFFER_SIZE_BYTES; index++)
    {
        display->framebuffer[index] = color;
    }
}

/*
 * Convert a logical landscape coordinate into the vendor RAM layout.
 * Logical X follows the 250-pixel long side, while logical Y uses only the
 * 122 visible pixels. The controller RAM itself is still 128 x 250.
 */
void epd_gdey0213b74_set_pixel(epd_gdey0213b74_t *display, uint16_t x_point, uint16_t y_point, uint8_t black)
{
    uint16_t x_ram;
    uint16_t y_ram;
    uint32_t address;
    uint8_t mask;

    if (epd_gdey0213b74_validate_display(display) != EPD_GDEY0213B74_OK)
    {
        return;
    }

    if ((x_point >= EPD_GDEY0213B74_CANVAS_WIDTH_PIXELS) ||
        (y_point >= EPD_GDEY0213B74_CANVAS_HEIGHT_PIXELS))
    {
        return;
    }

    /* Rotate logical coordinates by 270 degrees to match the panel RAM layout. */
    x_ram = y_point;
    y_ram = (uint16_t)(EPD_GDEY0213B74_RAM_HEIGHT_PIXELS - x_point - 1U);
    address = (uint32_t)(x_ram / 8U) + ((uint32_t)y_ram * EPD_GDEY0213B74_RAM_WIDTH_BYTES);
    mask = (uint8_t)(0x80U >> (x_ram % 8U));

    if (black != 0U)
    {
        display->framebuffer[address] &= (uint8_t)(~mask);
    }
    else
    {
        display->framebuffer[address] |= mask;
    }
}

/*
 * Filled rectangles are the only primitive needed by the current demo scene.
 * Text rendering reuses this function by turning each foreground glyph pixel
 * into a small filled block.
 */
void epd_gdey0213b74_fill_rect(epd_gdey0213b74_t *display,
                               uint16_t x_start,
                               uint16_t y_start,
                               uint16_t width,
                               uint16_t height,
                               uint8_t black)
{
    uint16_t x_offset;
    uint16_t y_offset;

    if (epd_gdey0213b74_validate_display(display) != EPD_GDEY0213B74_OK)
    {
        return;
    }

    for (y_offset = 0U; y_offset < height; y_offset++)
    {
        for (x_offset = 0U; x_offset < width; x_offset++)
        {
            epd_gdey0213b74_set_pixel(display,
                                      (uint16_t)(x_start + x_offset),
                                      (uint16_t)(y_start + y_offset),
                                      black);
        }
    }
}
