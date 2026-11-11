#ifndef STORAGE_LOG_H
#define STORAGE_LOG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

void storage_log_init(void);
/* Touches the SD card, so call from a task: the card's timers need the scheduler running */
void storage_log_mount(void);
/* Really writes to the card, because the socket has no card-detect pin. Also recovers a card
 * that was refitted. */
bool storage_sd_is_ok(void);
void storage_log_timer_step_10ms(void);
void storage_log_drain_step(void);
void storage_log_flush(void);
void storage_power_off_mark(void);
void storage_log_power_off_mark(void);

/* Verbose debug logger */
void storage_log_debug(const char *tag, const char *msg);
void storage_log_http(const char *entry);

/* One CSV line per 15-minute slot in SLOTS.CSV, for auditing gaps */
void storage_log_slot(const char *csv_line);
int storage_erase_station_data(void);
void storage_state_delete(const char *name);
/* Adds one timestamped line to a file on the card */
void storage_append_stamped(const char *name, const char *text);

/* One flash error log entry to ERRORS.TXT. At 64 KB the file becomes ERRORS.OLD, replacing the
 * previous one, so the two together never pass 128 KB. False if there's no usable card. */
bool storage_errors_append(const char *text, unsigned repeats);

/* Small state files. Saves overwrite in place, so keep each file's text a fixed length. */
bool storage_state_load(const char *filename, char *out, size_t size);
bool storage_state_save(const char *filename, const char *text);

/* 15-minute raw sensor archive */
bool storage_archive_slot(const char *day_file, const char *record);
/* True when the last storage_archive_slot failed because the card is full */
bool storage_card_full(void);

/* Backlog Management on SD Card */
void storage_backlog_init(void);
bool storage_backlog_save(const char *slot_time, const char *payload);
uint32_t storage_backlog_get_pending_count(void);
bool storage_backlog_get_next_unsent(uint32_t start_offset, uint32_t *out_offset, char *out_payload, size_t max_len);
bool storage_backlog_mark_sent(uint32_t entry_offset);
bool storage_backlog_note_rejected(uint32_t entry_offset, int strikes_allowed, const char *payload);
void storage_backlog_cleanup_if_all_sent(void);
void storage_backlog_clear_all(void);

#endif /* STORAGE_LOG_H */

