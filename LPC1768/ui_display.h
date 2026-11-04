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

/* Activity timer reset on user interactions */
void ui_reset_activity_timer(void);
bool ui_is_operator_active(void);

/* Backlog tracking */
uint32_t ui_get_backlog_count(void);
void ui_set_backlog_count(uint32_t count);
void ui_inc_backlog_count(void);
void ui_dec_backlog_count(void);

#endif /* UI_DISPLAY_H */

