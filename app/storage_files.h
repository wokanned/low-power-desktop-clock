#ifndef STORAGE_FILES_H
#define STORAGE_FILES_H

#include "file_store.h"

/* Copies the entire request into a static mailbox. No borrowed buffers.
 * Nonzero token required. At most one outstanding file request, including its
 * retained result. A caller must take the result before issuing the next one.
 * No IO in request/take/snapshot; task context only. */
int32_t storage_file_request(const file_store_request_t *p_request);
uint8_t storage_file_take_result(uint32_t token, file_store_response_t *p_response);
void storage_get_files_snapshot(file_store_snapshot_t *p_snapshot);
/* debugTask owns maintenance. Reserve before entering a binary session; retain
 * through EXIT/timeout upload cleanup. Individual text requests reserve their
 * own transaction automatically. No IO and no wait in these functions. */
int32_t storage_maintenance_acquire(void);
void storage_maintenance_release(void);
/* Asynchronous scan-only pause at a sector boundary. This does not acknowledge
 * quiescence and does not make suspending this task or powering down Flash safe.
 * MCU STOP2 uses the storage owner's POWER_BLOCK_STORAGE acknowledgment after
 * synchronous operations and Flash-idle checks; scan pause alone is insufficient.
 * Flash power removal/deep-power-down is not implemented. */
void storage_set_scan_paused(uint8_t paused);

#endif
