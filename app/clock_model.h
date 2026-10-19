#ifndef CLOCK_MODEL_H
#define CLOCK_MODEL_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    CLOCK_DATA_UNAVAILABLE = 0,
    CLOCK_DATA_INITIALIZING,
    CLOCK_DATA_READY,
    CLOCK_DATA_ERROR
} clock_data_state_t;

typedef enum { CLOCK_FORMAT_12 = 12, CLOCK_FORMAT_24 = 24 } clock_format_t;
typedef enum { CLOCK_MEMORY_SRAM = 0, CLOCK_MEMORY_IFLASH,
               CLOCK_MEMORY_W25Q, CLOCK_MEMORY_COUNT } clock_memory_mode_t;

typedef struct {
    uint16_t year;
    uint8_t month, day, weekday, hour, minute, second;
} clock_calendar_t;

typedef struct {
    clock_data_state_t state;
    int32_t value;
} clock_field_t;

typedef enum {
    CLOCK_FAULT_RTC = 0, CLOCK_FAULT_TEMP, CLOCK_FAULT_BAT,
    CLOCK_FAULT_WIFI, CLOCK_FAULT_IMU, CLOCK_FAULT_BARO,
    CLOCK_FAULT_LIGHT, CLOCK_FAULT_W25Q, CLOCK_FAULT_CONFIG,
    CLOCK_FAULT_TASK, CLOCK_FAULT_DEBUG, CLOCK_FAULT_SOURCE_COUNT
} clock_fault_source_t;

typedef enum {
    CLOCK_CATEGORY_RTC = 0, CLOCK_CATEGORY_TEMP, CLOCK_CATEGORY_BAT,
    CLOCK_CATEGORY_WIFI, CLOCK_CATEGORY_SENSOR, CLOCK_CATEGORY_STORAGE,
    CLOCK_CATEGORY_SYSTEM, CLOCK_CATEGORY_COUNT
} clock_fault_category_t;

#define CLOCK_FAULT_IO       (1UL << 0)
#define CLOCK_FAULT_CONFIGURATION (1UL << 1)
#define CLOCK_FAULT_TIMEOUT  (1UL << 2)
#define CLOCK_FAULT_ID       (1UL << 3)
#define CLOCK_FAULT_VERIFY   (1UL << 4)
#define CLOCK_FAULT_START    (1UL << 5)
#define CLOCK_FAULT_TX       (1UL << 6)
#define CLOCK_FAULT_RX       (1UL << 7)
#define CLOCK_FAULT_OVERFLOW (1UL << 8)

typedef struct {
    uint32_t active_mask;
    uint32_t first_mask;
    uint32_t first_tick;
    clock_calendar_t first_time;
    uint8_t time_valid;
} clock_fault_source_record_t;

typedef struct {
    uint32_t source_bits;
    uint32_t first_tick;
    clock_calendar_t first_time;
    uint8_t time_valid;
} clock_fault_category_record_t;

typedef struct {
    clock_fault_source_record_t source[CLOCK_FAULT_SOURCE_COUNT];
    clock_fault_category_record_t category[CLOCK_CATEGORY_COUNT];
    uint32_t revision;
    uint8_t active_categories;
} clock_fault_snapshot_t;

typedef struct {
    clock_calendar_t calendar;
    clock_data_state_t calendar_state;
    clock_format_t format;
    clock_memory_mode_t memory_mode;
    clock_field_t temperature; /* tenths of degrees Celsius */
    clock_field_t battery;     /* percent */
    clock_field_t wifi;        /* elapsed whole hours */
    clock_field_t memory[CLOCK_MEMORY_COUNT]; /* percent */
    clock_fault_snapshot_t faults;
    uint32_t format_revision;
    uint8_t startup_ready;
} clock_view_model_t;

/* Gregorian calendar, 2000..2099; weekday is 1=Monday through 7=Sunday. */
uint8_t clock_calendar_valid(const clock_calendar_t *p_calendar);
uint8_t clock_calendar_weekday(uint16_t year, uint8_t month, uint8_t day);
void clock_calendar_add_seconds(clock_calendar_t *p_calendar, uint32_t seconds);
const char *clock_weekday_label(uint8_t weekday);
const char *clock_fault_source_label(clock_fault_source_t source);
const char *clock_fault_category_label(clock_fault_category_t category);
const char *clock_fault_reason_label(uint32_t mask);
clock_fault_category_t clock_fault_category(clock_fault_source_t source);

#endif
