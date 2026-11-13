#ifndef UI_DISPLAY_H
#define UI_DISPLAY_H

#include "intermcu_protocol.h"
#include "sim7600_driver.h"
#include <stdbool.h>
#include <stdint.h>

/* Keypad Keys */
typedef enum {
    KEY_NONE = 0,
    KEY_MENU,       /* Row 0, Col 0 */
    KEY_UP,         /* Row 0, Col 1 (Up) */
    KEY_TOP = KEY_UP,
    KEY_CALIB,      /* Row 0, Col 2 */
    KEY_LEFT,       /* Row 1, Col 0 */
    KEY_EN,         /* Row 1, Col 1 (Enter/Set) */
    KEY_RIGHT,      /* Row 1, Col 2 */
    KEY_BACK,       /* Row 2, Col 0 */
    KEY_DOWN,       /* Row 2, Col 1 */
    KEY_DSP         /* Row 2, Col 2 (Wake / Sleep Display) */
} key_code_t;

void ui_display_init(void);
void ui_display_task_step(const telemetry_data_t *telemetry, const modem_status_t *modem_stat);
key_code_t ui_keypad_scan(void);

/* Activity timer and operator presence state */
void ui_reset_activity_timer(void);
bool ui_is_operator_active(void);
bool ui_is_display_active(void);
bool ui_is_operator_touched(void);
void ui_enter_sleep(void);

/* Boot type, known once the LPC845's first status frame arrives (within about 3s) */
bool ui_is_boot_mode_known(void);
bool ui_was_scheduled_boot(void);

/* While a slot is running the UI may not power the system down */
void ui_set_slot_busy(bool busy);

/* Implemented by the slot scheduler in main.c */
bool slot_scheduler_is_due(void);
/* MENU > SET DATE/TIME: sets the clock, restarts the rain-day total at 0, re-arms the scheduler */
uint32_t clock_set_by_hand(const char *ts14);
/* ERASE DATA (keypad, and VARSHA ERASE_DATA <ID> by SMS): rain total, backlog, record log, Last Logged */
void data_erase_all(void);
/* VARSHA ERASE_DATA: done by the main task after the slot is recorded (or at once with no slot running) */
void data_erase_request(void);
/* An ERASE_DATA by SMS waiting for the main task: the UI holds its power save meanwhile */
bool data_erase_pending(void);

/* Backlog tracking */
uint32_t ui_get_backlog_count(void);
void ui_set_backlog_count(uint32_t count);
void ui_inc_backlog_count(void);
void ui_dec_backlog_count(void);

#endif /* UI_DISPLAY_H */

