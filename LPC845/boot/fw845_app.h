/* What the LPC845 firmware adds to run under the bootloader */
#ifndef FW845_APP_H
#define FW845_APP_H

#include <stdint.h>

#define FW845_VERSION "845-v1.5"

void fw845_app_init(void);

/* From the main loop: keeps the bootloader's watchdog from restarting this chip */
void fw845_app_feed_watchdog(void);

#endif /* FW845_APP_H */
