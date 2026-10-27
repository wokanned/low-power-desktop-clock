#ifndef TEST_RTC_H
#define TEST_RTC_H
#include <stdint.h>

typedef enum { HAL_OK = 0, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
typedef enum { HAL_UNLOCKED = 0, HAL_LOCKED } HAL_LockTypeDef;
typedef enum { HAL_RTC_STATE_RESET = 0, HAL_RTC_STATE_READY,
    HAL_RTC_STATE_BUSY, HAL_RTC_STATE_TIMEOUT, HAL_RTC_STATE_ERROR } HAL_RTCStateTypeDef;
typedef struct {
    volatile uint32_t TR, DR, CR, ISR, PRER, WUTR, CALIBR, ALRMAR, ALRMBR;
    volatile uint32_t WPR, SSR, SHIFTR, TSTR, TSDR, TSSSR, CALR;
    volatile uint32_t TAFCR, ALRMASSR, ALRMBSSR, OR, BKP0R, BKP1R;
} RTC_TypeDef;
typedef struct {
    RTC_TypeDef *Instance;
    HAL_LockTypeDef Lock;
    volatile HAL_RTCStateTypeDef State;
} RTC_HandleTypeDef;
typedef struct {
    uint8_t Hours, Minutes, Seconds, TimeFormat;
    uint32_t SubSeconds, SecondFraction, DayLightSaving, StoreOperation;
} RTC_TimeTypeDef;
typedef struct { uint8_t WeekDay, Month, Date, Year; } RTC_DateTypeDef;

extern RTC_TypeDef g_fake_rtc;
extern RTC_HandleTypeDef hrtc;
#define RTC (&g_fake_rtc)
#define RTC_FORMAT_BIN 0U
#define RTC_CR_FMT (1UL << 6)
#define RTC_ISR_INIT (1UL << 7)
#define RTC_ISR_INITF (1UL << 6)
#define RTC_TR_HU_Pos 16U
#define RTC_TR_MNU_Pos 8U
#define RTC_TR_SU_Pos 0U
#define RTC_DR_YU_Pos 16U
#define RTC_DR_MU_Pos 8U
#define RTC_DR_DU_Pos 0U
#define RTC_DR_WDU_Pos 13U
#define RTC_TR_RESERVED_MASK 0x007F7F7FUL
#define RTC_DR_RESERVED_MASK 0x00FFFF3FUL
#define __HAL_UNLOCK(handle) ((handle)->Lock = HAL_UNLOCKED)
#define __HAL_RTC_WRITEPROTECTION_DISABLE(handle) test_rtc_write_protection((handle), 0U)
#define __HAL_RTC_WRITEPROTECTION_ENABLE(handle) test_rtc_write_protection((handle), 1U)

void test_rtc_write_protection(RTC_HandleTypeDef *p_handle, uint8_t enabled);
HAL_StatusTypeDef RTC_EnterInitMode(RTC_HandleTypeDef *p_handle);
HAL_StatusTypeDef RTC_ExitInitMode(RTC_HandleTypeDef *p_handle);
uint8_t RTC_ByteToBcd2(uint8_t value);
HAL_StatusTypeDef HAL_RTC_GetTime(RTC_HandleTypeDef *p_handle,
    RTC_TimeTypeDef *p_time, uint32_t format);
HAL_StatusTypeDef HAL_RTC_GetDate(RTC_HandleTypeDef *p_handle,
    RTC_DateTypeDef *p_date, uint32_t format);
#endif
