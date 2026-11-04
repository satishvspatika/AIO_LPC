#ifndef STORAGE_LOG_H
#define STORAGE_LOG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

void storage_log_init(void);
void storage_log_timer_step_10ms(void);
void storage_log_drain_step(void);
void storage_log_flush(void);

/* Verbose debug logger */
void storage_log_debug(const char *tag, const char *msg);
void storage_log_http(const char *entry);

/* 15-minute raw sensor archive */
bool storage_archive_slot(const char *date6, const char *record);

/* Backlog Management on SD Card */
void storage_backlog_init(void);
bool storage_backlog_save(const char *slot_time, const char *payload);
uint32_t storage_backlog_get_pending_count(void);
bool storage_backlog_get_next_unsent(uint32_t start_offset, uint32_t *out_offset, char *out_payload, size_t max_len);
bool storage_backlog_mark_sent(uint32_t entry_offset);
void storage_backlog_cleanup_if_all_sent(void);
void storage_backlog_clear_all(void);

#endif /* STORAGE_LOG_H */

