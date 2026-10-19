                                                                                                                                                                            #ifndef DEBUG_H
#define DEBUG_H

#include "vofa.h"

/* Initialize after UART setup, before the scheduler starts. */
void Debug_Init(void);
bool Debug_IsReady(void);
void StartDebugTask(void *p_argument);
/* Nonblocking copy; intentional console suppression succeeds without queueing. */
bool Debug_SendFloats(const float *p_data, uint8_t ch_cnt);
bool Debug_SendFrame(const void *p_data, uint8_t ch_cnt,
                     const VOFA_Data_TypeDef_Enum *p_types);
/* Compatibility hook: unsolicited text is never mixed with JustFloat. */
void Debug_Log(const char *p_msg);
uint32_t Debug_GetQueueDropCount(void);
uint32_t Debug_GetUartErrorCount(void);
uint32_t Debug_GetSuppressedCount(void);
bool Debug_IsConsoleActive(void);

#endif
