/* Isolate existing storage/scan regression tests from filesystem IO.
 * The real littlefs engine is exercised by run_file_store_host.ps1. */
#include "file_store.h"
#include <string.h>
void file_store_init(const w25q128_ctx_t *p_context,
                     void (*mark_dirty)(uint32_t, uint32_t))
{
    (void)p_context;
    (void)mark_dirty;
}
void file_store_step(void) { }
uint8_t file_store_execute(const file_store_request_t *p_request,
                           file_store_response_t *p_response)
{
    memset(p_response, 0, sizeof(*p_response));
    p_response->token = p_request->token;
    p_response->operation = p_request->operation;
    p_response->size = p_request->size;
    return 0U;
}
uint8_t file_store_poll(file_store_response_t *p_response)
{
    (void)p_response;
    return 0U;
}
void file_store_get_snapshot(file_store_snapshot_t *p_snapshot)
{
    memset(p_snapshot, 0, sizeof(*p_snapshot));
    p_snapshot->state = FILE_STORE_UNAVAILABLE;
    p_snapshot->total_blocks = FILE_STORE_BLOCK_COUNT;
}
