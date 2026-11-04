#ifndef STORAGE_LOG_H
#define STORAGE_LOG_H

#include <stdint.h>
#include <stdbool.h>

void storage_log_init(void);
void storage_log_timer_step_10ms(void);
void storage_log_drain_step(void);

/* Verbose debug logger */
void storage_log_debug(const char *tag, const char *msg);
void storage_log_http(const char *entry);

/* 15-minute raw sensor archive */
bool storage_archive_slot(const char *date6, const char *record);

#endif /* STORAGE_LOG_H */

