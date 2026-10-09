#include "epd_gdey029t94.h"

#include <stddef.h>
#include <string.h>

/*
 * 设备层：
 * 只实现 GDEY029T94 命令协议和状态机。
 * 不依赖 STM32、CubeMX、HAL 句柄或具体引脚名称。
 */
#define EPD_CMD_DRIVER_OUTPUT_CONTROL       0x01U
#define EPD_CMD_DEEP_SLEEP                  0x10U
#define EPD_CMD_DATA_ENTRY_MODE             0x11U
#define EPD_CMD_SW_RESET                    0x12U
#define EPD_CMD_TEMPERATURE_SENSOR_CONTROL  0x18U
#define EPD_CMD_WRITE_TEMPERATURE_REGISTER  0x1AU
#define EPD_CMD_DISPLAY_UPDATE_CONTROL_1    0x21U
#define EPD_CMD_DISPLAY_UPDATE_CONTROL_2    0x22U
#define EPD_CMD_MASTER_ACTIVATION           0x20U
#define EPD_CMD_WRITE_RAM                   0x24U
#define EPD_CMD_BORDER_WAVEFORM_CONTROL     0x3CU
#define EPD_CMD_SET_RAM_X_WINDOW            0x44U
#define EPD_CMD_SET_RAM_Y_WINDOW            0x45U
#define EPD_CMD_SET_RAM_X_COUNTER           0x4EU
#define EPD_CMD_SET_RAM_Y_COUNTER           0x4FU
#define EPD_CMD_WRITE_RAM_OLD               0x26U

#define EPD_DRIVER_OUTPUT_LAST_GATE_LOW     0x27U
#define EPD_DRIVER_OUTPUT_LAST_GATE_HIGH    0x01U
#define EPD_DATA_ENTRY_DECREMENT_Y          0x01U
#define EPD_RAM_X_END                       0x0FU
#define EPD_BORDER_WAVEFORM_VALUE           0x05U
#define EPD_TEMPERATURE_SENSOR_INTERNAL     0x80U
#define EPD_FULL_UPDATE_CONTROL             0xF7U
#define EPD_FAST_UPDATE_CONTROL             0xC7U
#define EPD_PARTIAL_UPDATE_CONTROL          0xFFU
#define EPD_DEEP_SLEEP_CHECK_CODE           0x01U

#define EPD_FAST_TEMPERATURE_LOAD           0xB1U
#define EPD_FAST_TEMPERATURE_VALUE         0x64U
#define EPD_FAST_TEMPERATURE_COMMIT         0x91U
#define EPD_PARTIAL_BORDER_WAVEFORM         0x80U

#define EPD_RESET_LOW_TIME_MS               10U
#define EPD_RESET_HIGH_TIME_MS              10U
#define EPD_POST_SLEEP_DELAY_MS             100U
#define EPD_BUSY_TIMEOUT_MS                 10000U
#define EPD_SPI_TIMEOUT_MS                  1000U

static epd_gdey029t94_status_t epd_wait_ready(
    epd_gdey029t94_t *device,
    uint32_t timeout_ms);

static epd_gdey029t94_status_t epd_validate_io(
    const epd_gdey029t94_io_t *io)
{
    if ((io == NULL) ||
        (io->spi_write == NULL) ||
        (io->read_busy == NULL) ||
        (io->set_cs == NULL) ||
        (io->set_dc == NULL) ||
        (io->set_reset == NULL) ||
        (io->delay_ms == NULL) ||
        (io->get_tick_ms == NULL)) {
        return EPD_GDEY029T94_ERROR_ARGUMENT;
    }

    return EPD_GDEY029T94_OK;
}

static epd_gdey029t94_status_t epd_write(
    epd_gdey029t94_t *device,
    uint8_t dc_level,
    const uint8_t *data,
    uint32_t length)
{
    int32_t io_status;

    if ((device == NULL) || (data == NULL) || (length == 0U)) {
        return EPD_GDEY029T94_ERROR_ARGUMENT;
    }

    /* CS 只包围一次完整的命令或数据传输。 */
    device->io.set_dc(device->io.handle, dc_level);
    device->io.set_cs(device->io.handle, 0U);
    io_status = device->io.spi_write(device->io.handle,
                                     data,
                                     length,
                                     EPD_SPI_TIMEOUT_MS);
    device->io.set_cs(device->io.handle, 1U);

    if (io_status != 0) {
        return EPD_GDEY029T94_ERROR_IO;
    }

    return EPD_GDEY029T94_OK;
}

static epd_gdey029t94_status_t epd_write_command(
    epd_gdey029t94_t *device,
    uint8_t command)
{
    return epd_write(device, 0U, &command, 1U);
}

static epd_gdey029t94_status_t epd_write_data(
    epd_gdey029t94_t *device,
    const uint8_t *data,
    uint32_t length)
{
    return epd_write(device, 1U, data, length);
}

static epd_gdey029t94_status_t epd_write_command_data(
    epd_gdey029t94_t *device,
    uint8_t command,
    const uint8_t *data,
    uint32_t length)
{
    epd_gdey029t94_status_t status;

    status = epd_write_command(device, command);
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }

    return epd_write_data(device, data, length);
}

static epd_gdey029t94_status_t epd_validate_frame(
    const uint8_t *image,
    uint32_t image_length)
{
    if (image == NULL) {
        return EPD_GDEY029T94_ERROR_ARGUMENT;
    }
    if (image_length != EPD_GDEY029T94_FRAME_BUFFER_SIZE) {
        return EPD_GDEY029T94_ERROR_BUFFER_SIZE;
    }
    return EPD_GDEY029T94_OK;
}

static epd_gdey029t94_status_t epd_validate_partial_window(
    const epd_gdey029t94_rect_t *window,
    const uint8_t *image,
    uint32_t image_length,
    uint32_t *expected_length)
{
    uint32_t window_bytes;

    if ((window == NULL) || (image == NULL) ||
        (expected_length == NULL)) {
        return EPD_GDEY029T94_ERROR_ARGUMENT;
    }
    if ((window->width == 0U) || (window->height == 0U)) {
        return EPD_GDEY029T94_ERROR_GEOMETRY;
    }
    if ((window->x >= EPD_GDEY029T94_WIDTH_PIXELS) ||
        (window->y >= EPD_GDEY029T94_HEIGHT_PIXELS)) {
        return EPD_GDEY029T94_ERROR_GEOMETRY;
    }
    if (((window->x % EPD_GDEY029T94_X_ALIGNMENT_PIXELS) != 0U) ||
        ((window->width % EPD_GDEY029T94_X_ALIGNMENT_PIXELS) != 0U)) {
        return EPD_GDEY029T94_ERROR_ALIGNMENT;
    }
    if (((uint32_t)window->x + window->width >
         EPD_GDEY029T94_WIDTH_PIXELS) ||
        ((uint32_t)window->y + window->height >
         EPD_GDEY029T94_HEIGHT_PIXELS)) {
        return EPD_GDEY029T94_ERROR_GEOMETRY;
    }

    window_bytes = window->width / EPD_GDEY029T94_X_ALIGNMENT_PIXELS;
    *expected_length = window_bytes * window->height;
    if (image_length != *expected_length) {
        return EPD_GDEY029T94_ERROR_BUFFER_SIZE;
    }

    return EPD_GDEY029T94_OK;
}

static epd_gdey029t94_status_t epd_trigger_update(
    epd_gdey029t94_t *device,
    uint8_t update_control)
{
    epd_gdey029t94_status_t status;

    status = epd_write_command_data(device,
                                    EPD_CMD_DISPLAY_UPDATE_CONTROL_2,
                                    &update_control,
                                    1U);
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }

    status = epd_write_command(device, EPD_CMD_MASTER_ACTIVATION);
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }

    return epd_wait_ready(device, EPD_BUSY_TIMEOUT_MS);
}

static epd_gdey029t94_status_t epd_set_partial_window(
    epd_gdey029t94_t *device,
    const epd_gdey029t94_rect_t *window)
{
    uint8_t ram_x_window[2];
    uint8_t ram_y_window[4];
    uint8_t ram_x_counter[1];
    uint8_t ram_y_counter[2];
    uint16_t x_start_byte;
    uint16_t x_end_byte;
    uint16_t controller_y_start;
    uint16_t controller_y_end;
    uint8_t partial_border[1];
    uint8_t data_entry[1];
    epd_gdey029t94_status_t status;

    x_start_byte = (uint16_t)(window->x /
                              EPD_GDEY029T94_X_ALIGNMENT_PIXELS);
    x_end_byte = (uint16_t)(x_start_byte +
                            (window->width /
                             EPD_GDEY029T94_X_ALIGNMENT_PIXELS) - 1U);
    /* 对外坐标按帧缓冲区从上到下排列；控制器 Y 采用递减方向，因此需要转换首尾地址。 */
    controller_y_start = (uint16_t)(EPD_GDEY029T94_HEIGHT_PIXELS - 1U -
                                    window->y);
    controller_y_end = (uint16_t)(EPD_GDEY029T94_HEIGHT_PIXELS -
                                  window->y - window->height);

    ram_x_window[0] = (uint8_t)x_start_byte;
    ram_x_window[1] = (uint8_t)x_end_byte;
    ram_y_window[0] = (uint8_t)(controller_y_start & 0xFFU);
    ram_y_window[1] = (uint8_t)(controller_y_start >> 8U);
    ram_y_window[2] = (uint8_t)(controller_y_end & 0xFFU);
    ram_y_window[3] = (uint8_t)(controller_y_end >> 8U);
    ram_x_counter[0] = (uint8_t)x_start_byte;
    ram_y_counter[0] = (uint8_t)(controller_y_start & 0xFFU);
    ram_y_counter[1] = (uint8_t)(controller_y_start >> 8U);
    partial_border[0] = EPD_PARTIAL_BORDER_WAVEFORM;
    data_entry[0] = EPD_DATA_ENTRY_DECREMENT_Y;

    status = epd_write_command_data(device,
                                    EPD_CMD_BORDER_WAVEFORM_CONTROL,
                                    partial_border,
                                    sizeof(partial_border));
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }
    status = epd_write_command_data(device,
                                    EPD_CMD_DATA_ENTRY_MODE,
                                    data_entry,
                                    sizeof(data_entry));
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }
    status = epd_write_command_data(device,
                                    EPD_CMD_SET_RAM_X_WINDOW,
                                    ram_x_window,
                                    sizeof(ram_x_window));
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }
    status = epd_write_command_data(device,
                                    EPD_CMD_SET_RAM_Y_WINDOW,
                                    ram_y_window,
                                    sizeof(ram_y_window));
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }
    status = epd_write_command_data(device,
                                    EPD_CMD_SET_RAM_X_COUNTER,
                                    ram_x_counter,
                                    sizeof(ram_x_counter));
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }
    return epd_write_command_data(device,
                                  EPD_CMD_SET_RAM_Y_COUNTER,
                                  ram_y_counter,
                                  sizeof(ram_y_counter));
}

static void epd_update_background_window(
    epd_gdey029t94_t *device,
    const epd_gdey029t94_rect_t *window,
    const uint8_t *image)
{
    uint32_t row;
    uint32_t window_bytes;
    uint32_t destination_offset;

    window_bytes = window->width / EPD_GDEY029T94_X_ALIGNMENT_PIXELS;
    for (row = 0U; row < window->height; row++) {
        destination_offset = ((uint32_t)(window->y + row) *
                              EPD_GDEY029T94_BYTES_PER_ROW) +
                             (window->x /
                              EPD_GDEY029T94_X_ALIGNMENT_PIXELS);
        /* 使用 memmove，允许调用者传入背景影像中的一段数据。 */
        memmove(&device->background_image[destination_offset],
                &image[row * window_bytes],
                window_bytes);
    }
}

static epd_gdey029t94_status_t epd_wait_ready(
    epd_gdey029t94_t *device,
    uint32_t timeout_ms)
{
    uint32_t start_ms;
    uint8_t busy_level;

    if (device == NULL) {
        return EPD_GDEY029T94_ERROR_ARGUMENT;
    }

    start_ms = device->io.get_tick_ms(device->io.handle);

    /* BUSY 轮询必须有截止时间，防止屏幕断开或异常时固件永久卡死。 */
    for (;;) {
        if (device->io.read_busy(device->io.handle, &busy_level) != 0) {
            return EPD_GDEY029T94_ERROR_IO;
        }

        if (busy_level == 0U) {
            return EPD_GDEY029T94_OK;
        }

        if ((uint32_t)(device->io.get_tick_ms(device->io.handle) - start_ms) >=
            timeout_ms) {
            return EPD_GDEY029T94_ERROR_BUSY_TIMEOUT;
        }

        device->io.delay_ms(device->io.handle, 1U);
    }
}

static epd_gdey029t94_status_t epd_hardware_reset(
    epd_gdey029t94_t *device)
{
    device->io.set_reset(device->io.handle, 0U);
    device->io.delay_ms(device->io.handle, EPD_RESET_LOW_TIME_MS);
    device->io.set_reset(device->io.handle, 1U);
    device->io.delay_ms(device->io.handle, EPD_RESET_HIGH_TIME_MS);

    return epd_wait_ready(device, EPD_BUSY_TIMEOUT_MS);
}

static epd_gdey029t94_status_t epd_configure_black_white(
    epd_gdey029t94_t *device)
{
    static const uint8_t driver_output[] = {
        EPD_DRIVER_OUTPUT_LAST_GATE_LOW,
        EPD_DRIVER_OUTPUT_LAST_GATE_HIGH,
        0x00U
    };
    static const uint8_t data_entry[] = {
        EPD_DATA_ENTRY_DECREMENT_Y
    };
    static const uint8_t ram_x_window[] = {
        0x00U,
        EPD_RAM_X_END
    };
    static const uint8_t ram_y_window[] = {
        EPD_DRIVER_OUTPUT_LAST_GATE_LOW,
        EPD_DRIVER_OUTPUT_LAST_GATE_HIGH,
        0x00U,
        0x00U
    };
    static const uint8_t border_waveform[] = {
        EPD_BORDER_WAVEFORM_VALUE
    };
    static const uint8_t update_control_1[] = {
        0x00U,
        0x80U
    };
    static const uint8_t temperature_sensor[] = {
        EPD_TEMPERATURE_SENSOR_INTERNAL
    };
    static const uint8_t ram_x_counter[] = {
        0x00U
    };
    static const uint8_t ram_y_counter[] = {
        EPD_DRIVER_OUTPUT_LAST_GATE_LOW,
        EPD_DRIVER_OUTPUT_LAST_GATE_HIGH
    };
    epd_gdey029t94_status_t status;

    /* 这些参数配置固定的 128 x 296 黑白屏几何尺寸和控制器 RAM 遍历方向。 */
    status = epd_write_command_data(device,
                                    EPD_CMD_DRIVER_OUTPUT_CONTROL,
                                    driver_output,
                                    sizeof(driver_output));
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }

    status = epd_write_command_data(device,
                                    EPD_CMD_DATA_ENTRY_MODE,
                                    data_entry,
                                    sizeof(data_entry));
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }

    status = epd_write_command_data(device,
                                    EPD_CMD_SET_RAM_X_WINDOW,
                                    ram_x_window,
                                    sizeof(ram_x_window));
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }

    status = epd_write_command_data(device,
                                    EPD_CMD_SET_RAM_Y_WINDOW,
                                    ram_y_window,
                                    sizeof(ram_y_window));
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }

    status = epd_write_command_data(device,
                                    EPD_CMD_BORDER_WAVEFORM_CONTROL,
                                    border_waveform,
                                    sizeof(border_waveform));
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }

    status = epd_write_command_data(device,
                                    EPD_CMD_DISPLAY_UPDATE_CONTROL_1,
                                    update_control_1,
                                    sizeof(update_control_1));
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }

    status = epd_write_command_data(device,
                                    EPD_CMD_TEMPERATURE_SENSOR_CONTROL,
                                    temperature_sensor,
                                    sizeof(temperature_sensor));
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }

    status = epd_write_command_data(device,
                                    EPD_CMD_SET_RAM_X_COUNTER,
                                    ram_x_counter,
                                    sizeof(ram_x_counter));
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }

    status = epd_write_command_data(device,
                                    EPD_CMD_SET_RAM_Y_COUNTER,
                                    ram_y_counter,
                                    sizeof(ram_y_counter));
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }

    return epd_wait_ready(device, EPD_BUSY_TIMEOUT_MS);
}

epd_gdey029t94_status_t epd_gdey029t94_init(
    epd_gdey029t94_t *device,
    const epd_gdey029t94_io_t *io)
{
    epd_gdey029t94_status_t status;

    if (device == NULL) {
        return EPD_GDEY029T94_ERROR_ARGUMENT;
    }

    status = epd_validate_io(io);
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }

    /* 复制回调到设备对象，后续操作不依赖全局硬件变量。 */
    memset(device, 0, sizeof(*device));
    device->io = *io;
    device->io.set_cs(device->io.handle, 1U);
    device->io.set_dc(device->io.handle, 0U);
    device->io.set_reset(device->io.handle, 1U);

    status = epd_hardware_reset(device);
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }

    status = epd_write_command(device, EPD_CMD_SW_RESET);
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }

    status = epd_wait_ready(device, EPD_BUSY_TIMEOUT_MS);
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }

    status = epd_configure_black_white(device);
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }

    device->initialized = 1U;
    device->sleeping = 0U;

    return EPD_GDEY029T94_OK;
}

epd_gdey029t94_status_t epd_gdey029t94_full_refresh(
    epd_gdey029t94_t *device,
    const uint8_t *image,
    uint32_t image_length)
{
    epd_gdey029t94_status_t status;

    if (device == NULL) {
        return EPD_GDEY029T94_ERROR_ARGUMENT;
    }
    status = epd_validate_frame(image, image_length);
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }
    if ((device->initialized == 0U) || (device->sleeping != 0U)) {
        return EPD_GDEY029T94_ERROR_STATE;
    }

    /* 之前的局刷可能留下小窗口选择；发送完整图像前恢复整屏 RAM 几何配置。 */
    status = epd_configure_black_white(device);
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }
    status = epd_write_command_data(device,
                                    EPD_CMD_WRITE_RAM,
                                    image,
                                    image_length);
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }
    /* 同步控制器旧图 RAM；该完整图像可以作为后续差分/局刷的有效基准。 */
    status = epd_write_command_data(device,
                                    EPD_CMD_WRITE_RAM_OLD,
                                    image,
                                    image_length);
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }
    status = epd_trigger_update(device, EPD_FULL_UPDATE_CONTROL);
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }

    memmove(device->background_image, image, image_length);
    device->background_valid = 1U;
    device->fast_initialized = 0U;
    device->refreshes_since_full = 0U;
    device->full_refreshes++;

    return EPD_GDEY029T94_OK;
}

epd_gdey029t94_status_t epd_gdey029t94_fast_init(
    epd_gdey029t94_t *device)
{
    static const uint8_t temperature_sensor[] = {
        EPD_TEMPERATURE_SENSOR_INTERNAL
    };
    static const uint8_t temperature_value[] = {
        EPD_FAST_TEMPERATURE_VALUE,
        0x00U
    };
    epd_gdey029t94_status_t status;

    if (device == NULL) {
        return EPD_GDEY029T94_ERROR_ARGUMENT;
    }
    if ((device->initialized == 0U) || (device->sleeping != 0U)) {
        return EPD_GDEY029T94_ERROR_STATE;
    }

    device->fast_initialized = 0U;
    status = epd_hardware_reset(device);
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }
    status = epd_write_command(device, EPD_CMD_SW_RESET);
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }
    status = epd_wait_ready(device, EPD_BUSY_TIMEOUT_MS);
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }

    /* 先明确配置屏幕几何参数，再执行原厂快刷温度流程：
       B1 -> 0x1A(0x64,0x00) -> 91。 */
    status = epd_configure_black_white(device);
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }
    status = epd_write_command_data(device,
                                    EPD_CMD_TEMPERATURE_SENSOR_CONTROL,
                                    temperature_sensor,
                                    sizeof(temperature_sensor));
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }
    status = epd_trigger_update(device, EPD_FAST_TEMPERATURE_LOAD);
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }
    status = epd_write_command_data(device,
                                    EPD_CMD_WRITE_TEMPERATURE_REGISTER,
                                    temperature_value,
                                    sizeof(temperature_value));
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }
    status = epd_trigger_update(device, EPD_FAST_TEMPERATURE_COMMIT);
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }

    device->fast_initialized = 1U;
    return EPD_GDEY029T94_OK;
}

epd_gdey029t94_status_t epd_gdey029t94_fast_refresh(
    epd_gdey029t94_t *device,
    const uint8_t *image,
    uint32_t image_length)
{
    epd_gdey029t94_status_t status;

    if (device == NULL) {
        return EPD_GDEY029T94_ERROR_ARGUMENT;
    }
    status = epd_validate_frame(image, image_length);
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }
    if ((device->initialized == 0U) || (device->sleeping != 0U)) {
        return EPD_GDEY029T94_ERROR_STATE;
    }
    /* 如果上一次更新后的清残影全刷失败，先重试全刷，再允许下一次会产生残影的更新。 */
    if (device->refreshes_since_full >= EPD_GDEY029T94_CLEANUP_INTERVAL) {
        status = epd_gdey029t94_full_refresh(
            device,
            device->background_image,
            sizeof(device->background_image));
        if (status != EPD_GDEY029T94_OK) {
            return status;
        }
    }
    if (device->fast_initialized == 0U) {
        status = epd_gdey029t94_fast_init(device);
        if (status != EPD_GDEY029T94_OK) {
            return status;
        }
    }

    /* 前一次局刷可能留下小窗口选择，快刷前恢复整屏 RAM 几何配置。 */
    status = epd_configure_black_white(device);
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }
    status = epd_write_command_data(device,
                                    EPD_CMD_WRITE_RAM,
                                    image,
                                    image_length);
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }
    status = epd_trigger_update(device, EPD_FAST_UPDATE_CONTROL);
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }

    /* 快刷显示完成后同步旧图 RAM，使后续局刷以当前实际显示图像为比较基准。 */
    status = epd_configure_black_white(device);
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }
    status = epd_write_command_data(device,
                                    EPD_CMD_WRITE_RAM_OLD,
                                    image,
                                    image_length);
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }
    memmove(device->background_image, image, image_length);
    device->background_valid = 1U;
    device->fast_refreshes++;
    device->refreshes_since_full++;

    /* 快刷和局刷共用一个残影计数；达到阈值后立即对最新完整图像执行全刷。 */
    if (device->refreshes_since_full >= EPD_GDEY029T94_CLEANUP_INTERVAL) {
        return epd_gdey029t94_full_refresh(device,
                                          device->background_image,
                                          sizeof(device->background_image));
    }

    return EPD_GDEY029T94_OK;
}

epd_gdey029t94_status_t epd_gdey029t94_set_background(
    epd_gdey029t94_t *device,
    const uint8_t *image,
    uint32_t image_length)
{
    /* 已同步 0x24/0x26 的完整图像，是局刷所需的背景基准。 */
    return epd_gdey029t94_full_refresh(device, image, image_length);
}

epd_gdey029t94_status_t epd_gdey029t94_partial_refresh(
    epd_gdey029t94_t *device,
    const epd_gdey029t94_rect_t *window,
    const uint8_t *image,
    uint32_t image_length)
{
    uint32_t expected_length;
    epd_gdey029t94_status_t status;

    if (device == NULL) {
        return EPD_GDEY029T94_ERROR_ARGUMENT;
    }
    /* 在第一次硬件写入前检查边界、字节对齐和精确缓冲区长度。 */
    status = epd_validate_partial_window(window,
                                         image,
                                         image_length,
                                         &expected_length);
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }
    if ((device->initialized == 0U) || (device->sleeping != 0U) ||
        (device->background_valid == 0U)) {
        return EPD_GDEY029T94_ERROR_STATE;
    }
    if (device->refreshes_since_full >= EPD_GDEY029T94_CLEANUP_INTERVAL) {
        status = epd_gdey029t94_full_refresh(
            device,
            device->background_image,
            sizeof(device->background_image));
        if (status != EPD_GDEY029T94_OK) {
            return status;
        }
    }

    /* 原厂局刷流程在每个窗口更新前复位；控制器图像 RAM 在该复位后保留，
       因此背景有效标志和主机侧影像仍然有效。 */
    status = epd_hardware_reset(device);
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }
    device->fast_initialized = 0U;
    status = epd_set_partial_window(device, window);
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }
    status = epd_write_command_data(device,
                                    EPD_CMD_WRITE_RAM,
                                    image,
                                    expected_length);
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }
    status = epd_trigger_update(device, EPD_PARTIAL_UPDATE_CONTROL);
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }

    epd_update_background_window(device, window, image);
    device->partial_refreshes++;
    device->refreshes_since_full++;
    if (device->refreshes_since_full >= EPD_GDEY029T94_CLEANUP_INTERVAL) {
        return epd_gdey029t94_full_refresh(device,
                                          device->background_image,
                                          sizeof(device->background_image));
    }

    return EPD_GDEY029T94_OK;
}

epd_gdey029t94_status_t epd_gdey029t94_get_refresh_stats(
    const epd_gdey029t94_t *device,
    epd_gdey029t94_refresh_stats_t *stats)
{
    if ((device == NULL) || (stats == NULL)) {
        return EPD_GDEY029T94_ERROR_ARGUMENT;
    }

    stats->full_refreshes = device->full_refreshes;
    stats->fast_refreshes = device->fast_refreshes;
    stats->partial_refreshes = device->partial_refreshes;
    stats->refreshes_since_full = device->refreshes_since_full;

    return EPD_GDEY029T94_OK;
}

epd_gdey029t94_status_t epd_gdey029t94_sleep(
    epd_gdey029t94_t *device)
{
    static const uint8_t sleep_code[] = {
        EPD_DEEP_SLEEP_CHECK_CODE
    };
    epd_gdey029t94_status_t status;

    if (device == NULL) {
        return EPD_GDEY029T94_ERROR_ARGUMENT;
    }

    if ((device->initialized == 0U) || (device->sleeping != 0U)) {
        return EPD_GDEY029T94_ERROR_STATE;
    }

    /* 单次刷新接口使用休眠；连续时钟模式保持屏幕唤醒。 */
    status = epd_write_command_data(device,
                                    EPD_CMD_DEEP_SLEEP,
                                    sleep_code,
                                    sizeof(sleep_code));
    if (status != EPD_GDEY029T94_OK) {
        return status;
    }

    device->io.delay_ms(device->io.handle, EPD_POST_SLEEP_DELAY_MS);
    device->sleeping = 1U;

    return EPD_GDEY029T94_OK;
}
