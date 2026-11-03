#ifndef IMU_ADXL345_H
#define IMU_ADXL345_H

#include <stdint.h>

#define ADXL345_ADDRESS_7BIT 0x53U
#define ADXL345_SCALE_G_PER_LSB 0.0039f
#define ADXL345_DATA_READY 0x80U
#define ADXL345_OVERRUN 0x01U
#define ADXL345_ACTIVITY 0x10U

typedef enum {
    ADXL345_OK = 0, ADXL345_NOT_READY = 1,
    ADXL345_ERROR_ARGUMENT = -1, ADXL345_ERROR_IO = -2,
    ADXL345_ERROR_ID = -3, ADXL345_ERROR_CONFIG = -4
} adxl345_status_t;

typedef struct {
    int32_t (*read_reg)(void *p_handle, uint8_t reg, uint8_t *p_data, uint16_t len);
    int32_t (*write_reg)(void *p_handle, uint8_t reg, const uint8_t *p_data, uint16_t len);
    void *handle;
} adxl345_ctx_t;

typedef struct {
    int16_t raw[3];
    uint8_t source;
} adxl345_sample_t;

/* Fixed first-version configuration: full resolution, +/-2 g, 100 Hz, FIFO bypass. */
adxl345_status_t adxl345_init(const adxl345_ctx_t *p_ctx);
/* Following init: AC-coupled XYZ activity, 0.25 g, active-high INT1.
 * Returns any previously latched source to the sole application owner, including
 * when a later configuration operation fails. No hidden discarded source read. */
adxl345_status_t adxl345_arm_activity(const adxl345_ctx_t *p_ctx, uint8_t *p_source);
/* Read-only health check of ID and the complete activity configuration. */
adxl345_status_t adxl345_check_activity(const adxl345_ctx_t *p_ctx);
/* Reading INT_SOURCE acknowledges latched activity. Sole owner only. */
adxl345_status_t adxl345_read_interrupt(const adxl345_ctx_t *p_ctx, uint8_t *p_source);
/* Reads INT_SOURCE, then all six data bytes in one transaction. source is valid
 * after a successful source read even for NOT_READY or a later burst IO error. */
adxl345_status_t adxl345_sample(const adxl345_ctx_t *p_ctx, adxl345_sample_t *p_sample);

#endif
