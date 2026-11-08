/* The application's side of firmware update: proves a new image healthy on its first boot, and
 * records how each update ended. */
#ifndef FW_APP_H
#define FW_APP_H

#include <stdint.h>
#include <stdbool.h>

/* Logs what the last update did. Call once, with the log ready. */
void fw_app_init(void);

/* Once a second. Confirms an image on trial after it has run a while and hears the LPC845. */
void fw_app_step(uint32_t uptime_sec, uint32_t good_frames);

/* Once a second, from the disk task. Firmware on trial is left to the bootloader's watchdog. Otherwise
 * the watchdog runs with a 60 s timeout, fed only while every registered task keeps checking in; a
 * task that stops is logged, then the unit restarts. */
void fw_app_watchdog_step(void);

/* Tasks the watchdog step watches. Each calls wd_checkin() at least this often (seconds): */
typedef enum {
    WD_MAIN = 0,    /* 360: a slot's longest step, modem start-up, sits well inside this */
    WD_UI,          /* 60 */
    WD_MODEM,       /* 180 */
    WD_COUNT
} wd_task_t;
void wd_register(wd_task_t id, void *task_handle);
void wd_checkin(void);   /* Counts for whichever registered task calls it */

#define STUCK_MAGIC 0xDEADC0D6UL  /* In GPREG0 after a restart for a task that stopped */

/* What the last update did, if it finished on this boot. NULL otherwise. */
const char *fw_app_last_result(void);

/* What the LPC845's last update did, if that was found on this boot. NULL otherwise. */
const char *fw_app_ctrl_last_result(void);

/* The LPC845's firmware version as it reported it on this boot ("845-v1.5"), "" until known */
const char *fw_app_845_version(void);

/* The running image's CRC32, to spot a file that is the same firmware */
uint32_t fw_app_running_crc(void);

#endif /* FW_APP_H */
