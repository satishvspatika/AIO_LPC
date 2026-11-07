#ifndef ERRLOG_H
#define ERRLOG_H

#include <stdbool.h>

/* Errors kept in flash, so a unit brought in for repair still has its history with no SD card or
 * a failed one. Copied to ERRORS.TXT whenever a card is present. */

/* Finds where the log left off. Call once at boot, before the tasks start. */
void errlog_init(void);

/* Any task: queues one error line for writing. msg is the text without the time, used to spot the
 * same error repeating. */
void errlog_note(const char *line, const char *msg);

/* Disk task, once a second: writes what was queued, and copies new entries to the card */
void errlog_step(void);

/* Only one entry waits at a time. Before an error that must not be missed, such as just before
 * powering off, wait for this to be true (the disk task writes within a second). */
bool errlog_idle(void);

#endif /* ERRLOG_H */
