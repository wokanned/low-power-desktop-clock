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
/* Asynchronous scan-only pause at a sector boundary. This does not acknowledge
 * quiescence and does not make suspending this task or powering down Flash safe.
 * A future PowerMgr must obtain an owner-side quiescent handshake. */
void storage_set_scan_paused(uint8_t paused);

#endif
