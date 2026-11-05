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

/* Once a second, from a task that keeps running: only feeds if the bootloader started the watchdog
 * and the image has confirmed itself. */
void fw_app_watchdog_step(void);

/* What the last update did, if it finished on this boot. NULL otherwise. */
const char *fw_app_last_result(void);

/* The running image's CRC32, to spot a file that is the same firmware */
uint32_t fw_app_running_crc(void);

#endif /* FW_APP_H */
