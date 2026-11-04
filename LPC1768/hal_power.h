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

/* RTC Synchronization with LPC845 DS1338 */
void hal_power_sync_rtc(const char *timestamp14);

/* Rail state interrogation */
bool hal_power_is_5v_on(void);
bool hal_power_is_modem_on(void);

#endif /* HAL_POWER_H */

