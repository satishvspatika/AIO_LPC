#ifndef HAL_POWER_H
#define HAL_POWER_H

#include <stdint.h>
#include <stdbool.h>

/* Initializes Inter-MCU UART3 and power management state */
void hal_power_init(void);

/* Power rail controls via LPC845 */
void hal_power_lcd_on(void);
void hal_power_lcd_off(void);
void hal_power_modem_on(void);
void hal_power_modem_off(void);
void hal_power_sleep_system(void);

/* Tells the LPC845 this chip is still making progress (it cuts power after 11 min without) */
void hal_power_heartbeat(void);

/* Erase Data: restart the LPC845's rain-day total */
void hal_power_rain_reset(void);

/* Field calibration: LPC845 counts tips separately, reporting them every second */
void hal_power_calib_start(void);
void hal_power_calib_stop(void);

/* Ask the LPC845 for its stored 15-minute sample (YYYYMMDDHHMMSS) */
void hal_power_request_record(const char *timestamp14);

/* RTC Synchronization with LPC845 DS1338 */
void hal_power_sync_rtc(const char *timestamp14);

/* Rail state interrogation */
bool hal_power_is_5v_on(void);
bool hal_power_is_modem_on(void);

#endif /* HAL_POWER_H */

