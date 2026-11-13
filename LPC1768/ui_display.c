#include "ui_display.h"
#include "hal_power.h"
#include "sim7600_driver.h"
#include "flash_nvram.h"
#include "intermcu_protocol.h"
#include "storage_log.h"
#include "telemetry_formatter.h"
#include "flash_log.h"
#include "fw_update.h"
#include "fw_ctrl.h"
#include "fw_app.h"
#include "ctrl_isp.h"
#include "boot/fw_journal.h"
#include "user_config.h"
#include "lpc17xx_gpio.h"
#include "lpc17xx_pinsel.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

/* --- HD44780 16x2 LCD Pin Definitions (Port 2) --- */
#define LCD_PORT        2
#define PIN_LCD_RS      4
#define PIN_LCD_RW      3
#define PIN_LCD_EN      2
#define PIN_LCD_D4      9
#define PIN_LCD_D5      6
#define PIN_LCD_D6      7
#define PIN_LCD_D7      8
#define PIN_LCD_BL      11

#define MASK_LCD_RS     (1UL << PIN_LCD_RS)
#define MASK_LCD_RW     (1UL << PIN_LCD_RW)
#define MASK_LCD_EN     (1UL << PIN_LCD_EN)
#define MASK_LCD_D4     (1UL << PIN_LCD_D4)
#define MASK_LCD_D5     (1UL << PIN_LCD_D5)
#define MASK_LCD_D6     (1UL << PIN_LCD_D6)
#define MASK_LCD_D7     (1UL << PIN_LCD_D7)
#define MASK_LCD_BL     (1UL << PIN_LCD_BL)

/* --- 3x3 Keypad Matrix Hardware Interface (Exact Hardware Pinout) ---
 * Rows (GPIO Outputs, driven LOW one at a time):
 *   output_row(0): Clears P3.26 LOW -> reads P0.22 (MENU),  P0.21 (UP),   P0.20 (CALIB)
 *   output_row(1): Clears P3.25 LOW -> reads P0.22 (LEFT),  P0.21 (EN),   P0.20 (RIGHT)
 *   output_row(2): Clears P1.21 LOW -> reads P0.22 (BACK),  P0.21 (DOWN), P0.20 (DSP)
 *
 * Columns (GPIO Inputs with internal pull-ups on Port 0):
 *   Col 0: Port 0, Pin 22 (Left Column:   MENU,  LEFT,  BACK)
 *   Col 1: Port 0, Pin 21 (Center Column: UP,    EN,    DOWN)
 *   Col 2: Port 0, Pin 20 (Right Column:  CALIB, RIGHT, DSP)
 */
#define KBD_ROW0_PORT   3
#define KBD_ROW0_PIN    26
#define KBD_ROW0_MASK   (1UL << 26)

#define KBD_ROW1_PORT   3
#define KBD_ROW1_PIN    25
#define KBD_ROW1_MASK   (1UL << 25)

#define KBD_ROW2_PORT   1
#define KBD_ROW2_PIN    21
#define KBD_ROW2_MASK   (1UL << 21)

#define KBD_COL0_PORT   0
#define KBD_COL0_PIN    22
#define KBD_COL0_MASK   (1UL << 22)

#define KBD_COL1_PORT   0
#define KBD_COL1_PIN    21
#define KBD_COL1_MASK   (1UL << 21)

#define KBD_COL2_PORT   0
#define KBD_COL2_PIN    20
#define KBD_COL2_MASK   (1UL << 20)

/* Screen Type Enumeration */
typedef enum {
    SCR_STATION_ID = 0,
    SCR_DATETIME,
    SCR_RAINFALL,
    SCR_TEMP_HUM,
    SCR_WIND,
    SCR_BATTERY,
    SCR_SIGNAL,
    SCR_LOCATION,
    SCR_LAST_LOGGED,
    SCR_BACKLOG,
    SCR_FIELD_CALIB,
    SCR_SEND_STATUS,
    SCR_ERASE_DATA,
    SCR_SLEEP,
    SCR_FIRMWARE
} screen_type_t;

/* Screens per System Configuration:
 * 1. KSNDMC_TWS: NO rain gauge / RF fields. Full weather sensors.
 * 2. KSNDMC_TRG: NO weather sensors. ONLY cumulative rain. Field Calib.
 * 3. BIHAR_TRG:  NO weather sensors. BOTH 15M and cumulative rain. Field Calib.
 * The last ones do something that matters on EN, so they open from MENU only. Power Save stays
 * in the scroll, as it harms nothing and the next slot wakes the unit.
 */
#if (UNIT_CFG == KSNDMC_TWS)
static const screen_type_t s_screens[] = {
    SCR_STATION_ID,
    SCR_FIRMWARE,       /* Both chips' versions, next to the landing page */
    SCR_DATETIME,
    SCR_TEMP_HUM,
    SCR_WIND,
    SCR_BATTERY,
    SCR_SIGNAL,
    SCR_LOCATION,
    SCR_LAST_LOGGED,
    SCR_BACKLOG,
    SCR_SLEEP,          /* Last in the scroll: one UP from the landing page */
    SCR_SEND_STATUS,
    SCR_ERASE_DATA
};
#define NUM_ACTION_SCREENS 2
#elif (UNIT_CFG == KSNDMC_TRG)
static const screen_type_t s_screens[] = {
    SCR_STATION_ID,
    SCR_FIRMWARE,       /* Both chips' versions, next to the landing page */
    SCR_DATETIME,
    SCR_RAINFALL,       /* Shows ONLY Cumulative Rainfall */
    SCR_BATTERY,
    SCR_SIGNAL,
    SCR_LOCATION,
    SCR_LAST_LOGGED,
    SCR_BACKLOG,
    SCR_FIELD_CALIB,    /* Last result, read-only: the CALIB key starts a run */
    SCR_SLEEP,          /* Last in the scroll: one UP from the landing page */
    SCR_SEND_STATUS,
    SCR_ERASE_DATA
};
#define NUM_ACTION_SCREENS 2
#else /* BIHAR_TRG */
static const screen_type_t s_screens[] = {
    SCR_STATION_ID,
    SCR_FIRMWARE,       /* Both chips' versions, next to the landing page */
    SCR_DATETIME,
    SCR_RAINFALL,       /* Shows BOTH 15M and Cumulative Rainfall */
    SCR_BATTERY,
    SCR_SIGNAL,
    SCR_LOCATION,
    SCR_LAST_LOGGED,
    SCR_BACKLOG,
    SCR_FIELD_CALIB,    /* Last result, read-only: the CALIB key starts a run */
    SCR_SLEEP,          /* Last in the scroll: one UP from the landing page */
    SCR_SEND_STATUS,
    SCR_ERASE_DATA
};
#define NUM_ACTION_SCREENS 2
#endif
#define NUM_SCREENS (sizeof(s_screens) / sizeof(s_screens[0]))
#define NUM_BROWSE_SCREENS (NUM_SCREENS - NUM_ACTION_SCREENS)

/* UI Operating Modes */
typedef enum {
    UI_MODE_BROWSE = 0,
    UI_MODE_EDIT_STN_ID,
    UI_MODE_EDIT_TIME,
    UI_MODE_CALIB,
    UI_MODE_ERASE_CONFIRM,
    UI_MODE_MENU,
    UI_MODE_SLEEP_CONFIRM,
    UI_MODE_KEY_TEST,
    UI_MODE_TEST_SEND,
    UI_MODE_FW_UPDATE,
    UI_MODE_GPS_CHOICE      /* Saved position: send it, or search for a new one first */
} ui_mode_t;

/* MENU key: everything that does something. Screen items jump to that screen, so actions such as
 * Send Status and Erase Data keep their own EN confirmation step. */
typedef enum {
    MENU_GOTO_SCREEN = 0,
    MENU_KEY_TEST,
    MENU_SET_STATION,
    MENU_SET_TIME,
    MENU_GPS,
    MENU_TEST_SEND,
    MENU_FW_UPDATE
} menu_action_t;

typedef struct {
    const char *label; /* Max 15 characters: shown after ">" on line 2 */
    menu_action_t action;
    screen_type_t screen;
} menu_item_t;

static const menu_item_t s_menu_items[] = {
    #if ENABLE_TEST_SEND
    { "TEST SEND",       MENU_TEST_SEND,   SCR_STATION_ID },
    #endif
    { "SEND STATUS SMS", MENU_GOTO_SCREEN, SCR_SEND_STATUS },
    /* One item (user, 2026-11-13): GPS UPDATE and SEND GPS SMS were two. The saved position is
     * sent as it is; a search only runs with no position yet, or when the operator asks (UP) */
    { "SEND GPS SMS",    MENU_GPS,         SCR_LOCATION },
    #if ENABLE_KEY_TEST
    { "KEY TEST",        MENU_KEY_TEST,    SCR_STATION_ID },
    #endif
    { "ERASE DATA",      MENU_GOTO_SCREEN, SCR_ERASE_DATA },
    { "FIRMWARE UPDATE", MENU_FW_UPDATE,   SCR_STATION_ID },
    { "SET DATE/TIME",   MENU_SET_TIME,    SCR_DATETIME },    /* Before the ID (user, 2026-11-13) */
    { "SET STATION ID",  MENU_SET_STATION, SCR_STATION_ID }
};
#define NUM_MENU_ITEMS (sizeof(s_menu_items) / sizeof(s_menu_items[0]))

#define SLEEP_CONFIRM_TIMEOUT_TICKS (10 * 20) /* 10s of 50ms UI ticks */
#define KEY_TEST_TIMEOUT_TICKS      (15 * 20) /* 15s of 50ms UI ticks */

/* Status SMS UI State */
typedef enum {
    STATUS_UI_IDLE = 0,
    STATUS_UI_SENDING,
    STATUS_UI_SUCCESS,
    STATUS_UI_FAILED
} status_ui_state_t;

static bool s_lcd_initialized = false;
static bool s_display_active = true;
static volatile bool s_system_asleep = false;
static volatile bool s_boot_mode_known = false;
static volatile bool s_scheduled_boot = false;
static volatile bool s_slot_busy = false;
static bool s_sleep_deferred = false;
static uint32_t s_boot_wait_ticks = 0;
static uint32_t s_lcd_rail_off_ticks = 0;
static bool s_operator_touched_key = false;
static uint32_t s_inactivity_ticks = 0; /* 50ms ticks */
static size_t s_current_screen_idx = 0;

static ui_mode_t s_ui_mode = UI_MODE_BROWSE;
static char s_edit_stn_id[STATION_ID_MAX + 1] = "";
static int s_edit_pos = 0;
static int s_edit_first_pos = 0; /* KSNDMC: 2 hides a leading "00", the stored ID stays 6 digits */

/* The ID padded to STATION_ID_DIGITS. Bihar shows all 8 digits; KSNDMC keeps the ESP32 rule,
 * a 6-digit ID starting "00" shows as its last 4 digits */
static const char *station_id_for_display(const char *id)
{
    static char padded[STATION_ID_MAX + 1];
    format_station_id(padded, sizeof(padded), id);
    if (STATION_ID_DIGITS == 6 && strncmp(padded, "00", 2) == 0) {
        return padded + 2;
    }
    return padded;
}
/* MENU > SET DATE/TIME (v6.9.34): DDMMYYYYHHMM, a digit at a time like the station ID, shown as
 * DD-MM-YYYY HH:MM. For sites with no network time (no SIM or coverage, a freshly converted unit).
 * Setting it starts afresh (user, 2026-11-12): rain-day total from 0, see clock_set_by_hand().
 * The server still corrects the clock on the next delivery if it is 90 s or more off. */
static char s_edit_time[13] = "";
static int s_time_pos = 0;
static const uint8_t s_time_col[12] = { 0, 1, 3, 4, 6, 7, 8, 9, 11, 12, 14, 15 };

/* A digit editor with the hardware cursor showing */
static bool ui_cursor_mode(void);

static size_t s_menu_pos = 0;
static ui_mode_t s_mode_before_confirm = UI_MODE_BROWSE;
static uint32_t s_mode_timeout_ticks = 0;
static key_code_t s_key_test_last = KEY_NONE;

/* Field Calibration (ESP32 flow): runs up to 5 minutes, exactly 10 tips is a pass */
#define CALIB_TIMEOUT_TICKS (5 * 60 * 20)
#define CALIB_READ_RESULT_SEC 60    /* After a finished run: 6 minutes in all before power saving */

/* Power saving after this long with no key. "Modem busy" doesn't extend it: while the LCD is on,
 * the supervisor takes the modem every couple of seconds. Real jobs have their own flags. */
#define UI_IDLE_SEC         90
#define UI_MODEM_FINISH_SEC 60      /* A supervisor step under way when the idle time ends gets this long */
#define UI_JOB_LIMIT_SEC    (8 * 60)    /* A stuck SMS, GPS or test job is cancelled after this */
#define UI_JOB_RESET_SEC    60      /* And the unit restarts if the cancel doesn't end it */
#define CALIB_PASS_TIPS     10
static uint32_t s_calib_ticks = 0;

/* Status SMS Feedback & Background Worker */
static status_ui_state_t s_status_sms_state = STATUS_UI_IDLE;
static uint32_t s_status_sms_timer = 0;
static xTaskHandle s_sms_task_handle = NULL;
static char s_status_sms_substep[20] = "PRESS EN TO SEND";
static uint32_t s_anim_counter = 0;

/* LCD Differential Render Cache & Redraw Flag */
static char s_cached_line1[17] = "";
static char s_cached_line2[17] = "";
static ui_mode_t s_cached_mode = (ui_mode_t)-1;
static int s_cached_edit_pos = -1;
static bool s_force_redraw = true;

/* HTTP Backlog Tracking */
static uint32_t s_backlog_count = 0;

uint32_t ui_get_backlog_count(void)
{
    return s_backlog_count;
}

void ui_set_backlog_count(uint32_t count)
{
    s_backlog_count = count;
}

void ui_inc_backlog_count(void)
{
    s_backlog_count++;
}

void ui_dec_backlog_count(void)
{
    if (s_backlog_count > 0) s_backlog_count--;
}

static void on_sms_progress(const char *step_text)
{
    if (step_text) {
        strncpy(s_status_sms_substep, step_text, sizeof(s_status_sms_substep) - 1);
        s_status_sms_substep[sizeof(s_status_sms_substep) - 1] = '\0';
    }
}

/* Short-lived jobs: their lowest free stack, before it's gone with the task */
static void log_job_stack(void)
{
    char msg[40];
    snprintf(msg, sizeof(msg), "Job stack free: %u words", (unsigned int)uxTaskGetStackHighWaterMark(NULL));
    storage_log_debug("STACK", msg);
}

static void vStatusSms_Task(void *pvParameters)
{
    (void)pvParameters;

    if (!sim7600_acquire(3000)) {
        s_status_sms_state = STATUS_UI_FAILED;
        strncpy(s_status_sms_substep, "MODEM BUSY      ", sizeof(s_status_sms_substep) - 1);
        s_status_sms_timer = 40;
        s_sms_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    telemetry_data_t t;
    memset(&t, 0, sizeof(t));
    intermcu_get_telemetry(&t);

    modem_status_t m;
    sim7600_get_status(&m);

    bool ok = sim7600_transmit_status_sms(&t, m.csq_dbm);

    sim7600_release();

    if (sim7600_is_cancel_requested()) {
        s_status_sms_state = STATUS_UI_IDLE;
        strncpy(s_status_sms_substep, "CANCELLED       ", sizeof(s_status_sms_substep) - 1);
        s_status_sms_timer = 20;
    } else {
        s_status_sms_state = ok ? STATUS_UI_SUCCESS : STATUS_UI_FAILED;
        if (ok) {
            strncpy(s_status_sms_substep, "STATUS SENT OK  ", sizeof(s_status_sms_substep) - 1);
        }
        s_status_sms_timer = 60; /* 3 seconds display */
    }

    log_job_stack();
    s_sms_task_handle = NULL;
    vTaskDelete(NULL);
}

#if ENABLE_TEST_SEND
/* TEMPORARY bench tool: sends one dummy record for a chosen past 15-minute slot, so the server
 * link can be tested without waiting for a slot. Uses its own station ID, and stays out of
 * flash, DATA/ and the backlog.
 * Remove with ENABLE_TEST_SEND once deliveries are proven. */
static volatile bool s_test_running = false;

/* TEST_DATE_HELPERS_BEGIN */
typedef enum { TEST_DAY = 0, TEST_MONTH, TEST_YEAR, TEST_HOUR, TEST_MIN, TEST_FIELD_COUNT } test_field_t;

static int s_test_year = 2026, s_test_month = 1, s_test_day = 1;
static int s_test_slot_min = 0;   /* Minutes since midnight, always on a 15-minute boundary */

static int test_days_in_month(int year, int month)
{
    static const unsigned char days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    return (month == 2 && leap) ? 29 : days[month - 1];
}

static void test_step_day(int dir)
{
    s_test_day += dir;
    if (s_test_day < 1) {
        if (--s_test_month < 1) {
            s_test_month = 12;
            s_test_year--;
        }
        s_test_day = test_days_in_month(s_test_year, s_test_month);
    } else if (s_test_day > test_days_in_month(s_test_year, s_test_month)) {
        s_test_day = 1;
        if (++s_test_month > 12) {
            s_test_month = 1;
            s_test_year++;
        }
    }
}

/* Moves by one 15-minute slot, carrying into the date */
static void test_step_slot(int dir)
{
    s_test_slot_min += dir * 15;
    if (s_test_slot_min < 0) {
        s_test_slot_min += 1440;
        test_step_day(-1);
    } else if (s_test_slot_min >= 1440) {
        s_test_slot_min -= 1440;
        test_step_day(1);
    }
}

/* UP/DOWN on one field. Only the minute field carries, so setting the others can't move them. */
static void test_adjust_field(test_field_t field, int dir)
{
    int max_day;
    switch (field) {
    case TEST_DAY:
        max_day = test_days_in_month(s_test_year, s_test_month);
        s_test_day += dir;
        if (s_test_day < 1) s_test_day = max_day;
        if (s_test_day > max_day) s_test_day = 1;
        break;
    case TEST_MONTH:
        s_test_month += dir;
        if (s_test_month < 1) s_test_month = 12;
        if (s_test_month > 12) s_test_month = 1;
        break;
    case TEST_YEAR:
        s_test_year += dir;
        if (s_test_year < 2020) s_test_year = 2020;
        if (s_test_year > 2099) s_test_year = 2099;
        break;
    case TEST_HOUR:
        s_test_slot_min += dir * 60;
        if (s_test_slot_min < 0) s_test_slot_min += 1440;
        if (s_test_slot_min >= 1440) s_test_slot_min -= 1440;
        break;
    default:
        test_step_slot(dir);
        break;
    }
    /* A shorter month or a non-leap year can leave the day out of range */
    max_day = test_days_in_month(s_test_year, s_test_month);
    if (s_test_day > max_day) s_test_day = max_day;
}
/* TEST_DATE_HELPERS_END */

static test_field_t s_test_field = TEST_MIN;
static const char *volatile s_test_status = NULL; /* NULL: show which field UP/DOWN changes */

static void vTestSend_Task(void *pvParameters)
{
    (void)pvParameters;

    char payload[256];
    char line[104];
    telemetry_data_t t;
    int slot_min = s_test_slot_min;
    bool sent = false;
    int csq = -113;
    portTickType start_tick = xTaskGetTickCount();

    memset(&t, 0, sizeof(t));
    intermcu_get_telemetry(&t);
    float bat = t.bat1_volt;

    /* The slot being tested, with values that are easy to spot on the server */
    snprintf(t.slot_time, sizeof(t.slot_time), "%04d%02d%02d%02d%02d00", s_test_year, s_test_month, s_test_day, slot_min / 60, slot_min % 60);
    t.temp_c = 25.5f;
    t.humidity_pct = 55.5f;
    t.wind_speed_avg = 1.5f;
    t.wind_dir_deg = 90;
    t.rf_15m_mm = 0.5f;
    t.rf_cum_mm = 1.0f;

    snprintf(line, sizeof(line), "Dummy record, station %s, slot %.12s, clock %.14s", TEST_STATION_ID, t.slot_time, t.present_time);
    storage_log_debug("TEST_START", line);

    if (!sim7600_acquire(5000)) {
        storage_log_debug("TEST_END", "Modem busy, nothing sent");
        s_test_status = "MODEM BUSY";
        s_test_running = false;
        s_sms_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    sim7600_clear_cancel();
    if (sim7600_power_on()) {
        modem_status_t m;
        sim7600_get_status(&m);
        csq = m.csq_dbm;

        #if (UNIT_CFG == KSNDMC_TRG)
        format_payload_ksndmc_trg(payload, sizeof(payload), TEST_STATION_ID, &t, csq);
        #elif (UNIT_CFG == BIHAR_TRG)
        format_payload_bihar_trg(payload, sizeof(payload), TEST_STATION_ID, &t, csq);
        #else
        format_payload_ksndmc_tws(payload, sizeof(payload), TEST_STATION_ID, &t, csq);
        #endif
        storage_log_debug("HTTP_POST", payload);

        sent = sim7600_http_post(telemetry_get_endpoint(UNIT_CFG), payload, NULL, 0);
        if (!sent) {
            sent = sim7600_net_diagnostics(telemetry_get_endpoint(UNIT_CFG), payload);
        }
    } else {
        storage_log_debug("TEST", "Modem did not come up");
    }
    sim7600_release();

    unsigned int duration = (unsigned int)((xTaskGetTickCount() - start_tick) / configTICK_RATE_HZ);
    snprintf(line, sizeof(line), "%s station=%s slot=%.12s csq=%d took=%us", sent ? "SENT" : "NOT_CONFIRMED", TEST_STATION_ID, t.slot_time, csq, duration);
    storage_log_debug("TEST_END", line);

    char s_bat[8];
    fmt_fixed(s_bat, sizeof(s_bat), bat, 4, 2);
    snprintf(line, sizeof(line), "%.14s,%.14s,%s,DUMMY_TEST,1,%d,%u,%s,TEST,0",
             t.present_time, t.slot_time, sent ? "SENT" : "FAILED", csq, duration, s_bat);
    storage_log_slot(line);

    /* A pass moves on, so each test uses a different slot. A failure stays, to retry it. */
    if (sent) {
        test_step_slot(-1);
    }
    s_test_status = sent ? "SENT OK EN=NEXT" : "FAILED EN=RETRY";
    s_test_running = false;
    log_job_stack();
    s_sms_task_handle = NULL;
    vTaskDelete(NULL);
}

static void test_send_open(const telemetry_data_t *telemetry)
{
    const char *p = telemetry->present_time;
    s_test_status = NULL;
    s_test_field = TEST_MIN;
    if (strlen(p) >= 12 && strncmp(p, "20", 2) == 0 && p[2] != '0') {
        s_test_year = (p[0] - '0') * 1000 + (p[1] - '0') * 100 + (p[2] - '0') * 10 + (p[3] - '0');
        s_test_month = (p[4] - '0') * 10 + (p[5] - '0');
        s_test_day = (p[6] - '0') * 10 + (p[7] - '0');
        int now_min = ((p[8] - '0') * 10 + (p[9] - '0')) * 60 + (p[10] - '0') * 10 + (p[11] - '0');
        /* The previous slot, unless it is under 2 minutes old: the server refuses a slot in its
         * first 62s, so then the one before it */
        s_test_slot_min = (now_min / 15) * 15;
        if (now_min - s_test_slot_min < 2) {
            test_step_slot(-1);
        }
    } else {
        s_test_status = "NO CLOCK:SET IT";
    }
    s_ui_mode = UI_MODE_TEST_SEND;
}

static void test_send_key(key_code_t key)
{
    if (s_test_running) {
        return; /* Locked until the send finishes */
    }
    if (key == KEY_BACK || key == KEY_MENU) {
        s_ui_mode = UI_MODE_BROWSE;
    } else if (key == KEY_LEFT) {
        s_test_field = (test_field_t)((s_test_field + TEST_FIELD_COUNT - 1) % TEST_FIELD_COUNT);
        s_test_status = NULL;
    } else if (key == KEY_RIGHT) {
        s_test_field = (test_field_t)((s_test_field + 1) % TEST_FIELD_COUNT);
        s_test_status = NULL;
    } else if (key == KEY_UP) {
        test_adjust_field(s_test_field, 1);
        s_test_status = NULL;
    } else if (key == KEY_DOWN) {
        test_adjust_field(s_test_field, -1);
        s_test_status = NULL;
    } else if (key == KEY_EN) {
        if (s_slot_busy || slot_scheduler_is_due()) {
            s_test_status = "SLOT DUE, WAIT";
        } else if (s_sms_task_handle != NULL) {
            s_test_status = "SMS BUSY, WAIT";
        } else {
            s_test_running = true;
            strncpy(s_status_sms_substep, "STARTING...", sizeof(s_status_sms_substep) - 1);
            if (xTaskCreate(vTestSend_Task, (const signed char *)"TestSend", 896, NULL, 2, &s_sms_task_handle) != pdPASS) {
                s_sms_task_handle = NULL;
                s_test_running = false;
                s_test_status = "NO MEMORY";
                storage_log_debug("ERROR", "Test send: no memory for its task");
            }
        }
    }
}
#endif /* ENABLE_TEST_SEND */

/* LOCATION screen: EN searches now. Shares the SMS task slot, as both use the modem. */
static volatile bool s_gps_running = false;
static const char *volatile s_gps_status = NULL;
static volatile bool s_gps_send_sms = false;            /* MENU > SEND GPS SMS: send after the search */
static volatile portTickType s_gps_status_tick = 0;     /* When s_gps_status was set */
#define GPS_STATUS_SHOW_TICKS (3000 / portTICK_RATE_MS)  /* With a position saved, LAT/LON come back after this */

static void gps_status_set(const char *text)
{
    s_gps_status_tick = xTaskGetTickCount();
    s_gps_status = text;
}
static char s_gps_today[12] = "";
static bool s_gps_search = true;    /* False: send the saved position, no search */
#define GPS_CHOICE_TIMEOUT_TICKS (20 * 20)  /* 20 s of 50 ms UI ticks */

static void vGps_Task(void *pvParameters)
{
    (void)pvParameters;

    if (!sim7600_acquire(3000)) {
        gps_status_set("MODEM BUSY");
    } else {
        sim7600_clear_cancel();
        /* GPS needs the modem on, not a network: a site with no signal can still get a position */
        bool registered = sim7600_power_on();
        if (!registered && !sim7600_modem_answered()) {
            gps_status_set("MODEM FAIL");
        } else {
            if (s_gps_search) {
                switch (sim7600_gps_update(s_gps_today)) {
                case GPS_NEW_POSITION:  gps_status_set("POSITION SAVED"); break;
                case GPS_MOVED:         gps_status_set("MOVED: UPDATED"); break;
                case GPS_SAME_POSITION: gps_status_set("SAME POSITION"); break;
                case GPS_NOT_AVAILABLE: gps_status_set("GPS NOT AVAIL"); break;
                default:                gps_status_set("NO FIX"); break;
                }
            }
            if (s_gps_send_sms) {
                /* F: asked from the field (keypad), saved or fresh. C is for office queries by SMS */
                static telemetry_data_t t;      /* GPS task stack is short */
                memset(&t, 0, sizeof(t));
                intermcu_get_telemetry(&t);
                strncpy(s_status_sms_substep, "SENDING GPS SMS ", sizeof(s_status_sms_substep) - 1);
                if (!registered) {
                    gps_status_set("NO NETWORK: SMS");
                } else if (!flash_nvram_get_location(NULL, NULL)) {
                    gps_status_set("NO POSITION");
                } else {
                    bool sent = sim7600_send_sms(STATUS_SMS_DEST_NUMBER, sim7600_build_gps_sms('F', t.present_time));
                    gps_status_set(sent ? "GPS SMS SENT" : "GPS SMS FAILED");
                }
            }
        }
        sim7600_release();
    }
    s_gps_running = false;
    log_job_stack();
    s_sms_task_handle = NULL;
    vTaskDelete(NULL);
}

static void gps_task_start(const telemetry_data_t *telemetry, bool send_sms, bool search)
{
    if (s_gps_running) {
        return;
    }
    s_gps_send_sms = send_sms;
    s_gps_search = search;
    if (s_slot_busy || slot_scheduler_is_due()) {
        gps_status_set("SLOT DUE, WAIT");
        return;
    }
    if (s_sms_task_handle != NULL) {
        gps_status_set("MODEM BUSY");
        return;
    }
    const char *t = telemetry->present_time;
    snprintf(s_gps_today, sizeof(s_gps_today), "%.4s-%.2s-%.2s", t, t + 4, t + 6);
    s_gps_status = NULL;
    s_gps_running = true;
    strncpy(s_status_sms_substep, "GPS STARTING... ", sizeof(s_status_sms_substep) - 1);
    if (xTaskCreate(vGps_Task, (const signed char *)"GpsTask", 896, NULL, 1, &s_sms_task_handle) != pdPASS) {
        s_sms_task_handle = NULL;
        s_gps_running = false;
        gps_status_set("NO MEMORY");
        storage_log_debug("ERROR", "Not enough memory to start the GPS task");
    }
}

/* Search for a position, save it, then (send_sms) send it */
static void gps_fetch_start(const telemetry_data_t *telemetry, bool send_sms)
{
    gps_task_start(telemetry, send_sms, true);
}

/* Send the saved position at once: no search */
static void gps_send_saved_start(const telemetry_data_t *telemetry)
{
    gps_task_start(telemetry, true, false);
}

/* MENU > FIRMWARE UPDATE. Nothing is erased until the operator has seen the checked file's version
 * and pressed EN. Shares the SMS task slot, as nothing else should use the modem or card meanwhile. */
#define FW_MIN_SECONDS_TO_SLOT 180

typedef enum { FW_UI_MENU = 0, FW_UI_BUSY, FW_UI_CONFIRM_TYPE, FW_UI_CONFIRM_MAIN, FW_UI_CONFIRM_REVERT, FW_UI_CONFIRM_CTRL, FW_UI_CONFIRM_ISP, FW_UI_MESSAGE } fw_ui_state_t;
typedef enum { FW_OP_CHECK_MAIN = 0, FW_OP_CHECK_CTRL, FW_OP_CHECK_REVERT, FW_OP_CHECK_TYPE, FW_OP_INSTALL_MAIN, FW_OP_INSTALL_CTRL, FW_OP_DO_REVERT, FW_OP_CTRL_ISP } fw_task_op_t;

static const char *const s_fw_menu[] = { "MAIN F/W", "CTRL F/W", "REVERT F/W", "CHANGE TYPE" };

/* One card carries a file per station type, and each unit takes its own. Another type's file is
 * only ever offered from CHANGE TYPE, so a shared card can't lead to an accidental change. */
static const char *fw_file_for(uint32_t profile)
{
    switch (profile) {
    case KSNDMC_TRG: return FW_FILE_KSNDMC_TRG;
    case BIHAR_TRG:  return FW_FILE_BIHAR_TRG;
    default:         return FW_FILE_KSNDMC_TWS;
    }
}
#define FW_MENU_ITEMS (sizeof(s_fw_menu) / sizeof(s_fw_menu[0]))

static volatile fw_ui_state_t s_fw_ui = FW_UI_MENU;
static volatile bool s_fw_running = false;
static fw_task_op_t s_fw_op = FW_OP_CHECK_MAIN;
static size_t s_fw_pos = 0;
static char s_fw_line1[17] = "";
static char s_fw_line2[17] = "";
static fw_file_info_t s_fw_info;
static const char *s_fw_file = NULL;
static int s_fw_type_tries = 0;

static const char *fw_type_name(uint32_t profile)
{
    switch (profile) {
    case KSNDMC_TRG: return "KSNDMC TRG";
    case BIHAR_TRG:  return "BIHAR TRG";
    case KSNDMC_TWS: return "KSNDMC TWS";
    default:         return "UNKNOWN";
    }
}

static void fw_show(fw_ui_state_t state, const char *line1, const char *line2)
{
    snprintf(s_fw_line1, sizeof(s_fw_line1), "%s", line1);
    snprintf(s_fw_line2, sizeof(s_fw_line2), "%s", line2);
    s_fw_ui = state;
}

static void fw_progress(const char *stage, unsigned percent)
{
    char text[20];
    snprintf(text, sizeof(text), "%s %u%%", stage, percent);
    strncpy(s_status_sms_substep, text, sizeof(s_status_sms_substep) - 1);
    s_status_sms_substep[sizeof(s_status_sms_substep) - 1] = '\0';
    hal_power_heartbeat(); /* Keeps the LPC845's hang timer quiet through a long copy */
}

static void fw_restart_for_install(const char *what)
{
    fw_show(FW_UI_BUSY, "INSTALLING...", "DO NOT POWER OFF");
    strncpy(s_status_sms_substep, "DO NOT POWER OFF", sizeof(s_status_sms_substep) - 1);
    storage_log_debug("FW", what);
    storage_log_flush();
    /* The LCD keeps showing this while the bootloader, which has no LCD code, does its work */
    vTaskDelay(1500 / portTICK_RATE_MS);
    NVIC_SystemReset();
}

static void vFwUpdate_Task(void *pvParameters)
{
    (void)pvParameters;
    char line[17];

    switch (s_fw_op) {
    case FW_OP_CHECK_MAIN: {
        s_fw_file = fw_file_for(UNIT_CFG);
        fw_result_t r = fw_update_check(s_fw_file, UNIT_CFG, &s_fw_info, fw_progress);
        if (r == FW_OK && s_fw_info.crc32 == fw_app_running_crc()) {
            /* The usual case for a card that has already updated this station. Reflashing is
             * allowed, but the operator should know that's what it is. */
            snprintf(line, sizeof(line), "SAME F/W %.7s", s_fw_info.version);
            fw_show(FW_UI_CONFIRM_MAIN, line, "EN=REDO BACK=NO");
        } else if (r == FW_OK) {
            snprintf(line, sizeof(line), "FW %.7s FOUND", s_fw_info.version);
            fw_show(FW_UI_CONFIRM_MAIN, line, "EN=YES  BACK=NO");
        } else if (r == FW_E_NOT_FOUND) {
            fw_show(FW_UI_MESSAGE, s_fw_file, "NOT FOUND");
        } else {
            storage_log_debug("ERROR", fw_update_error_text(r));
            fw_show(FW_UI_MESSAGE, "UPDATE REFUSED", fw_update_error_text(r));
        }
        break;
    }
    case FW_OP_CHECK_TYPE: {
        /* The other two types' files, in turn: BACK at the prompt moves on to the next */
        bool offered = false;
        while (s_fw_type_tries < 2 && !offered) {
            uint32_t profile = (uint32_t)((UNIT_CFG + 1 + s_fw_type_tries) % 3);
            s_fw_type_tries++;
            s_fw_file = fw_file_for(profile);
            fw_result_t r = fw_update_check(s_fw_file, profile, &s_fw_info, fw_progress);
            if (r == FW_OK) {
                snprintf(line, sizeof(line), "TO %.12s?", fw_type_name(profile));
                fw_show(FW_UI_CONFIRM_TYPE, line, "ERASES ALL DATA");
                offered = true;
            } else if (r != FW_E_NOT_FOUND) {
                storage_log_debug("ERROR", fw_update_error_text(r));
                fw_show(FW_UI_MESSAGE, "UPDATE REFUSED", fw_update_error_text(r));
                offered = true;
            }
        }
        if (!offered) {
            fw_show(FW_UI_MESSAGE, "NO OTHER TYPE", "FILE ON CARD");
        }
        break;
    }
    case FW_OP_CHECK_CTRL: {
        fw_ctrl_state_t state;
        fw_result_t r = fw_ctrl_check(FW_CTRL_FILENAME, &s_fw_info, fw_progress);
        if (r == FW_E_NOT_FOUND) {
            fw_show(FW_UI_MESSAGE, "CTRL.HEX", "NOT FOUND");
        } else if (r != FW_OK) {
            storage_log_debug("ERROR", fw_update_error_text(r));
            fw_show(FW_UI_MESSAGE, "UPDATE REFUSED", fw_update_error_text(r));
        } else if (!fw_ctrl_query(&state)) {
            /* No CTRL bootloader (a replaced board, blank or old firmware): it goes on through the
             * LPC845's ROM loader with JP1 + JP2 fitted, at the next boot (ctrl_isp.c) */
            storage_log_debug("FW", "CTRL chip doesn't answer: offering the ROM loader route (JP1+JP2)");
            fw_show(FW_UI_CONFIRM_ISP, "NO CTRL BOOTLDR", "FIT JP1+JP2, EN");
        } else if (state.state == FW_J_TRIAL_BOOT) {
            fw_show(FW_UI_MESSAGE, "CTRL ON TRIAL", "TRY AGAIN SOON");
        } else if (s_fw_info.crc32 == state.crc32) {
            snprintf(line, sizeof(line), "SAME CTRL %.6s", s_fw_info.version);
            fw_show(FW_UI_CONFIRM_CTRL, line, "EN=REDO BACK=NO");
        } else {
            snprintf(line, sizeof(line), "CTRL %.6s FOUND", s_fw_info.version);
            fw_show(FW_UI_CONFIRM_CTRL, line, "EN=YES  BACK=NO");
        }
        break;
    }
    case FW_OP_CTRL_ISP: {
        /* Done at the next boot, before the RTOS starts (ctrl_isp.c) */
        char msg[80];
        snprintf(msg, sizeof(msg), "CTRL bootloader + %s by the ROM loader: restarting to do it", s_fw_info.version);
        ctrl_isp_request(s_fw_info.crc32);
        fw_restart_for_install(msg);
        break;
    }
    case FW_OP_INSTALL_CTRL: {
        fw_result_t r = fw_ctrl_send_image(FW_CTRL_FILENAME, &s_fw_info, fw_progress);
        if (r == FW_OK) {
            fw_show(FW_UI_BUSY, "INSTALLING CTRL", "DO NOT POWER OFF");
            strncpy(s_status_sms_substep, "DO NOT POWER OFF", sizeof(s_status_sms_substep) - 1);
            storage_log_debug("FW", "New LPC845 firmware sent and checked, restarting it to install");
            storage_log_flush();
            fw_ctrl_restart_lpc845();
            /* The LPC845 cuts this chip's power as it restarts. If that hasn't happened, restart
             * anyway, so every case comes back the same way and checks how the update ended. */
            vTaskDelay(3000 / portTICK_RATE_MS);
            NVIC_SystemReset();
        }
        storage_log_debug("ERROR", fw_update_error_text(r));
        fw_show(FW_UI_MESSAGE, "UPDATE STOPPED", fw_update_error_text(r));
        break;
    }
    case FW_OP_CHECK_REVERT:
        if (fw_update_slot_b_info(&s_fw_info)) {
            snprintf(line, sizeof(line), "BACK TO %.7s", s_fw_info.version);
            fw_show(FW_UI_CONFIRM_REVERT, line, "EN=YES BACK=NO");
        } else {
            fw_show(FW_UI_MESSAGE, "NO PREVIOUS F/W", "CAN'T REVERT");
        }
        break;
    case FW_OP_INSTALL_MAIN: {
        fw_result_t r = fw_update_stage(s_fw_file, &s_fw_info, fw_progress);
        if (r == FW_OK) {
            fw_restart_for_install("New firmware staged and checked, restarting to install");
        }
        {
            char why[72];
            snprintf(why, sizeof(why), "Update stopped: %s (result %d, flash status %lu)", fw_update_error_text(r), (int)r,
                     (unsigned long)fw_hal_last_status);
            storage_log_debug("ERROR", why);
        }
        fw_show(FW_UI_MESSAGE, "UPDATE STOPPED", fw_update_error_text(r));
        break;
    }
    case FW_OP_DO_REVERT:
        if (fw_journal_append(FW_J_REVERT_REQ, FW_OP_NONE, 0)) {
            fw_restart_for_install("Going back to the previous firmware, restarting");
        }
        fw_show(FW_UI_MESSAGE, "REVERT STOPPED", "FLASH ERROR");
        break;
    default:
        break;
    }

    s_fw_running = false;
    log_job_stack();
    s_sms_task_handle = NULL;
    vTaskDelete(NULL);
}

static void fw_start_task(fw_task_op_t op, const char *first_step)
{
    s_fw_op = op;
    s_fw_running = true;
    s_fw_ui = FW_UI_BUSY;
    snprintf(s_fw_line1, sizeof(s_fw_line1), "FIRMWARE UPDATE");
    strncpy(s_status_sms_substep, first_step, sizeof(s_status_sms_substep) - 1);
    s_status_sms_substep[sizeof(s_status_sms_substep) - 1] = '\0';
    if (xTaskCreate(vFwUpdate_Task, (const signed char *)"FwTask", 896, NULL, 1, &s_sms_task_handle) != pdPASS) {
        s_sms_task_handle = NULL;
        s_fw_running = false;
        storage_log_debug("ERROR", "Not enough memory to start the firmware update task");
        fw_show(FW_UI_MESSAGE, "UPDATE STOPPED", "NO MEMORY");
    }
}

/* An update must not run into a slot, or on a weak battery */
static bool fw_safe_to_install(const telemetry_data_t *telemetry)
{
    const char *t = telemetry->present_time;
    if (s_slot_busy || s_sms_task_handle != NULL) {
        fw_show(FW_UI_MESSAGE, "UNIT BUSY", "TRY AGAIN SOON");
        return false;
    }
    if (strlen(t) >= 14) {
        int minute = (t[10] - '0') * 10 + (t[11] - '0');
        int second = (t[12] - '0') * 10 + (t[13] - '0');
        int to_slot = (15 - (minute % 15)) * 60 - second;
        if (to_slot < FW_MIN_SECONDS_TO_SLOT || slot_scheduler_is_due()) {
            fw_show(FW_UI_MESSAGE, "SLOT DUE SOON", "TRY AFTER SLOT");
            return false;
        }
    }
    /* Below 2 V the unit could not be running: the battery sense line is broken or unplugged, not the
     * battery flat (1922, new TWS cable, 0.23 V, 2026-11-09). Blocking would leave no way to install
     * the fix, so that one is logged and allowed; a real low battery still stops the update. */
    if (telemetry->bat1_volt < BATTERY_SENSE_FAULT_V) {
        storage_log_debug("ERROR", "Battery reads below 2 V (sense line?): updating anyway");
    } else if (telemetry->bat1_volt < GPS_MIN_BATTERY_V) {
        fw_show(FW_UI_MESSAGE, "BATTERY LOW", "NOT UPDATING");
        return false;
    }
    return true;
}

static void fw_open(void)
{
    s_fw_pos = 0;
    const char *last = fw_app_ctrl_last_result();
    if (!last) {
        last = fw_app_last_result();
    }
    if (last) {
        fw_show(FW_UI_MESSAGE, "LAST UPDATE:", last);
    } else {
        s_fw_ui = FW_UI_MENU;
    }
    s_ui_mode = UI_MODE_FW_UPDATE;
}

static void fw_key(key_code_t key, const telemetry_data_t *telemetry)
{
    if (s_fw_running || s_fw_ui == FW_UI_BUSY) {
        return; /* Locked until it finishes */
    }
    switch (s_fw_ui) {
    case FW_UI_MENU:
        if (key == KEY_UP || key == KEY_LEFT) {
            s_fw_pos = (s_fw_pos + FW_MENU_ITEMS - 1) % FW_MENU_ITEMS;
        } else if (key == KEY_DOWN || key == KEY_RIGHT) {
            s_fw_pos = (s_fw_pos + 1) % FW_MENU_ITEMS;
        } else if (key == KEY_BACK || key == KEY_MENU) {
            s_ui_mode = UI_MODE_BROWSE;
        } else if (key == KEY_EN) {
            if (s_sms_task_handle != NULL) {
                fw_show(FW_UI_MESSAGE, "UNIT BUSY", "TRY AGAIN SOON");
            } else if (s_fw_pos == 0) {
                fw_start_task(FW_OP_CHECK_MAIN, "CHECKING 0%");
            } else if (s_fw_pos == 1) {
                fw_start_task(FW_OP_CHECK_CTRL, "SEARCHING...");
            } else if (s_fw_pos == 2) {
                fw_start_task(FW_OP_CHECK_REVERT, "CHECKING...");
            } else {
                s_fw_type_tries = 0;
                fw_start_task(FW_OP_CHECK_TYPE, "SEARCHING...");
            }
        }
        break;
    case FW_UI_CONFIRM_TYPE:
        if (key == KEY_EN) {
            /* Second step: this is the same prompt as any update */
            char line[17];
            char msg[48];
            snprintf(line, sizeof(line), "FW %.7s FOUND", s_fw_info.version);
            fw_show(FW_UI_CONFIRM_MAIN, line, "EN=YES  BACK=NO");
            snprintf(msg, sizeof(msg), "Operator accepted station type %s", fw_type_name(s_fw_info.profile));
            storage_log_debug("FW", msg);
        } else if (key == KEY_BACK) {
            fw_start_task(FW_OP_CHECK_TYPE, "SEARCHING...");
        }
        break;
    case FW_UI_CONFIRM_ISP:
        if (key == KEY_EN) {
            if (!ctrl_isp_jumpers_fitted()) {
                fw_show(FW_UI_CONFIRM_ISP, "JP1+JP2 NOT ON", "FIT BOTH, EN");
            } else if (fw_safe_to_install(telemetry)) {
                fw_start_task(FW_OP_CTRL_ISP, "RESTARTING...");
            }
        } else if (key == KEY_BACK) {
            s_fw_ui = FW_UI_MENU;
        }
        break;
    case FW_UI_CONFIRM_MAIN:
    case FW_UI_CONFIRM_CTRL:
    case FW_UI_CONFIRM_REVERT:
        if (key == KEY_EN) {
            if (fw_safe_to_install(telemetry)) {
                if (s_fw_ui == FW_UI_CONFIRM_MAIN) {
                    fw_start_task(FW_OP_INSTALL_MAIN, "ERASING 0%");
                } else if (s_fw_ui == FW_UI_CONFIRM_CTRL) {
                    fw_start_task(FW_OP_INSTALL_CTRL, "ERASING 0%");
                } else {
                    fw_start_task(FW_OP_DO_REVERT, "RESTARTING...");
                }
            }
        } else if (key == KEY_BACK) {
            s_fw_ui = FW_UI_MENU;
        }
        break;
    default:
        if (key == KEY_EN || key == KEY_BACK) {
            s_fw_ui = FW_UI_MENU;
        }
        break;
    }
}

volatile uint32_t g_dbg_last_key = 0;
volatile uint32_t g_dbg_cols_r0 = 7;
volatile uint32_t g_dbg_cols_r1 = 7;
volatile uint32_t g_dbg_cols_r2 = 7;

static void render_screen(screen_type_t scr, const telemetry_data_t *telemetry, const modem_status_t *modem_stat);

static void delay_us(uint32_t us)
{
    volatile uint32_t count = us * 30;
    while (count--) { __asm volatile("nop"); }
}

static void lcd_pulse_en(void)
{
    delay_us(2);
    LPC_GPIO2->FIOSET = MASK_LCD_EN;
    delay_us(5);
    LPC_GPIO2->FIOCLR = MASK_LCD_EN;
    delay_us(80);
}

static void lcd_write_nibble(uint8_t nibble, bool is_data)
{
    if (!s_lcd_initialized) {
        return; /* Pins held low until the LCD is set up (lcd_pins_quiet) */
    }
    if (is_data) {
        LPC_GPIO2->FIOSET = MASK_LCD_RS;
    } else {
        LPC_GPIO2->FIOCLR = MASK_LCD_RS;
    }
    LPC_GPIO2->FIOCLR = MASK_LCD_RW;

    if (nibble & 0x01) LPC_GPIO2->FIOSET = MASK_LCD_D4; else LPC_GPIO2->FIOCLR = MASK_LCD_D4;
    if (nibble & 0x02) LPC_GPIO2->FIOSET = MASK_LCD_D5; else LPC_GPIO2->FIOCLR = MASK_LCD_D5;
    if (nibble & 0x04) LPC_GPIO2->FIOSET = MASK_LCD_D6; else LPC_GPIO2->FIOCLR = MASK_LCD_D6;
    if (nibble & 0x08) LPC_GPIO2->FIOSET = MASK_LCD_D7; else LPC_GPIO2->FIOCLR = MASK_LCD_D7;

    lcd_pulse_en();
}

static void lcd_write_byte(uint8_t byte, bool is_data)
{
    lcd_write_nibble((byte >> 4) & 0x0F, is_data);
    lcd_write_nibble(byte & 0x0F, is_data);
}

static void lcd_command(uint8_t cmd)
{
    lcd_write_byte(cmd, false);
    if (cmd == 0x01 || cmd == 0x02) {
        delay_us(3000);
    } else {
        delay_us(100);
    }
}

static void lcd_data(uint8_t data)
{
    lcd_write_byte(data, true);
    delay_us(80);
}

static void lcd_init_hw(void)
{
    PINSEL_CFG_Type pin_cfg;
    pin_cfg.Portnum = LCD_PORT;
    pin_cfg.Funcnum = PINSEL_FUNC_0;
    pin_cfg.Pinmode = PINSEL_PINMODE_PULLUP;
    pin_cfg.OpenDrain = PINSEL_PINMODE_NORMAL;

    const uint8_t lcd_pins[] = {PIN_LCD_RS, PIN_LCD_RW, PIN_LCD_EN, PIN_LCD_D4, PIN_LCD_D5, PIN_LCD_D6, PIN_LCD_D7, PIN_LCD_BL};
    for (size_t i = 0; i < sizeof(lcd_pins); i++) {
        pin_cfg.Pinnum = lcd_pins[i];
        PINSEL_ConfigPin(&pin_cfg);
    }

    LPC_GPIO2->FIODIR |= (MASK_LCD_RS | MASK_LCD_RW | MASK_LCD_EN | MASK_LCD_D4 | MASK_LCD_D5 | MASK_LCD_D6 | MASK_LCD_D7 | MASK_LCD_BL);
    LPC_GPIO2->FIOCLR = (MASK_LCD_RS | MASK_LCD_RW | MASK_LCD_EN | MASK_LCD_D4 | MASK_LCD_D5 | MASK_LCD_D6 | MASK_LCD_D7);
    LPC_GPIO2->FIOSET = MASK_LCD_BL;
    s_lcd_initialized = true;

    delay_us(60000);
    lcd_write_nibble(0x03, false);
    delay_us(5000);
    lcd_write_nibble(0x03, false);
    delay_us(200);
    lcd_write_nibble(0x03, false);
    delay_us(200);
    lcd_write_nibble(0x02, false);
    delay_us(200);

    lcd_command(0x28);
    delay_us(2000);
    lcd_command(0x08);
    delay_us(2000);
    lcd_command(0x01);
    delay_us(5000);
    lcd_command(0x06);
    delay_us(2000);
    lcd_command(0x0C);
    delay_us(2000);

    s_lcd_initialized = true;
    s_display_active = true;
    s_force_redraw = true;
    s_cached_line1[0] = '\0';
    s_cached_line2[0] = '\0';
    s_cached_mode = (ui_mode_t)-1;
    s_cached_edit_pos = -1;
}

static void lcd_set_cursor(uint8_t row, uint8_t col)
{
    uint8_t addr = (row == 0) ? (0x80 + col) : (0xC0 + col);
    lcd_command(addr);
}

static void lcd_print(const char *str)
{
    while (*str) {
        lcd_data((uint8_t)*str++);
    }
}

/* --- 3x3 Keypad Matrix Hardware Interface (Matching Golden TWS5 kbd.c / kbd.h) ---
 * Rows (GPIO Outputs, driven LOW one at a time):
 *   Row 0: Port 3, Pin 26 (Top Row:    MENU,  UP,    CALIB)
 *   Row 1: Port 3, Pin 25 (Middle Row: LEFT,  EN,    RIGHT)
 *   Row 2: Port 1, Pin 21 (Bottom Row: BACK,  DOWN,  DSP)
 *
 * Columns (GPIO Inputs with internal pull-ups on Port 0):
 *   Col 0: Port 0, Pin 22 (Left Column:   MENU,  LEFT,  BACK)
 *   Col 1: Port 0, Pin 21 (Center Column: UP,    EN,    DOWN)
 *   Col 2: Port 0, Pin 20 (Right Column:  CALIB, RIGHT, DSP)
 */
#define KBD_FIFO_SIZE 8
static volatile key_code_t s_kbd_fifo[KBD_FIFO_SIZE];
static volatile uint8_t s_kbd_head = 0;
static volatile uint8_t s_kbd_tail = 0;

static void kbd_fifo_put(key_code_t k)
{
    uint8_t next = (s_kbd_head + 1) % KBD_FIFO_SIZE;
    if (next != s_kbd_tail) {
        s_kbd_fifo[s_kbd_head] = k;
        s_kbd_head = next;
    }
}

key_code_t ui_keypad_get_key(void)
{
    if (s_kbd_head == s_kbd_tail) {
        return KEY_NONE;
    }
    key_code_t k = s_kbd_fifo[s_kbd_tail];
    s_kbd_tail = (s_kbd_tail + 1) % KBD_FIFO_SIZE;
    return k;
}

key_code_t ui_keypad_scan(void)
{
    return ui_keypad_get_key();
}

static void keypad_init_hw(void)
{
    PINSEL_CFG_Type pin_cfg;
    pin_cfg.Funcnum = PINSEL_FUNC_0;
    pin_cfg.Pinmode = PINSEL_PINMODE_PULLUP;
    pin_cfg.OpenDrain = PINSEL_PINMODE_NORMAL;

    /* Rows as GPIO Outputs (P3.25 = Row 0, P3.26 = Row 1, P1.21 = Row 2) */
    pin_cfg.Portnum = KBD_ROW0_PORT; pin_cfg.Pinnum = KBD_ROW0_PIN; PINSEL_ConfigPin(&pin_cfg);
    pin_cfg.Portnum = KBD_ROW1_PORT; pin_cfg.Pinnum = KBD_ROW1_PIN; PINSEL_ConfigPin(&pin_cfg);
    pin_cfg.Portnum = KBD_ROW2_PORT; pin_cfg.Pinnum = KBD_ROW2_PIN; PINSEL_ConfigPin(&pin_cfg);

    /* Columns as GPIO Inputs (P0.22 = Col 0, P0.21 = Col 1, P0.20 = Col 2) */
    pin_cfg.Portnum = KBD_COL0_PORT; pin_cfg.Pinnum = KBD_COL0_PIN; PINSEL_ConfigPin(&pin_cfg);
    pin_cfg.Portnum = KBD_COL1_PORT; pin_cfg.Pinnum = KBD_COL1_PIN; PINSEL_ConfigPin(&pin_cfg);
    pin_cfg.Portnum = KBD_COL2_PORT; pin_cfg.Pinnum = KBD_COL2_PIN; PINSEL_ConfigPin(&pin_cfg);

    /* Direct register configuration for maximum hardware reliability */
    LPC_PINCON->PINSEL7 &= ~((0x03UL << 18) | (0x03UL << 20)); /* P3.25, P3.26 */
    LPC_PINCON->PINMODE7 &= ~((0x03UL << 18) | (0x03UL << 20)); /* Pull-Up (00) */
    LPC_PINCON->PINMODE_OD3 &= ~(KBD_ROW0_MASK | KBD_ROW1_MASK);
    LPC_GPIO3->FIODIR |= (KBD_ROW0_MASK | KBD_ROW1_MASK);       /* Outputs */
    LPC_GPIO3->FIOMASK &= ~(KBD_ROW0_MASK | KBD_ROW1_MASK);     /* Unmask */
    LPC_GPIO3->FIOSET = (KBD_ROW0_MASK | KBD_ROW1_MASK);         /* Idle HIGH */

    LPC_PINCON->PINSEL3 &= ~(0x03UL << 10);                     /* P1.21 */
    LPC_PINCON->PINMODE3 &= ~(0x03UL << 10);                    /* Pull-Up (00) */
    LPC_PINCON->PINMODE_OD1 &= ~KBD_ROW2_MASK;
    LPC_GPIO1->FIODIR |= KBD_ROW2_MASK;                        /* Output */
    LPC_GPIO1->FIOMASK &= ~KBD_ROW2_MASK;                      /* Unmask */
    LPC_GPIO1->FIOSET = KBD_ROW2_MASK;                         /* Idle HIGH */

    LPC_PINCON->PINSEL1 &= ~((0x03UL << 8) | (0x03UL << 10) | (0x03UL << 12)); /* P0.20, P0.21, P0.22 */
    LPC_PINCON->PINMODE1 &= ~((0x03UL << 8) | (0x03UL << 10) | (0x03UL << 12)); /* Pull-Up (00) */
    LPC_PINCON->PINMODE_OD0 &= ~(KBD_COL0_MASK | KBD_COL1_MASK | KBD_COL2_MASK);
    LPC_GPIO0->FIODIR &= ~(KBD_COL0_MASK | KBD_COL1_MASK | KBD_COL2_MASK);      /* Inputs */
    LPC_GPIO0->FIOMASK &= ~(KBD_COL0_MASK | KBD_COL1_MASK | KBD_COL2_MASK);     /* Explicitly unmasked */
}

static void kbd_release_pins_for_sleep(void)
{
    PINSEL_CFG_Type pin_cfg;
    pin_cfg.Funcnum = PINSEL_FUNC_0;
    pin_cfg.Pinmode = PINSEL_PINMODE_TRISTATE;
    pin_cfg.OpenDrain = PINSEL_PINMODE_NORMAL;

    pin_cfg.Portnum = KBD_ROW0_PORT; pin_cfg.Pinnum = KBD_ROW0_PIN; PINSEL_ConfigPin(&pin_cfg);
    pin_cfg.Portnum = KBD_ROW1_PORT; pin_cfg.Pinnum = KBD_ROW1_PIN; PINSEL_ConfigPin(&pin_cfg);
    pin_cfg.Portnum = KBD_ROW2_PORT; pin_cfg.Pinnum = KBD_ROW2_PIN; PINSEL_ConfigPin(&pin_cfg);

    pin_cfg.Portnum = KBD_COL0_PORT; pin_cfg.Pinnum = KBD_COL0_PIN; PINSEL_ConfigPin(&pin_cfg);
    pin_cfg.Portnum = KBD_COL1_PORT; pin_cfg.Pinnum = KBD_COL1_PIN; PINSEL_ConfigPin(&pin_cfg);
    pin_cfg.Portnum = KBD_COL2_PORT; pin_cfg.Pinnum = KBD_COL2_PIN; PINSEL_ConfigPin(&pin_cfg);

    /* Direct register configuration to guarantee High-Z inputs with pull-ups disabled */
    LPC_GPIO3->FIODIR &= ~(KBD_ROW0_MASK | KBD_ROW1_MASK);
    LPC_GPIO1->FIODIR &= ~KBD_ROW2_MASK;
    LPC_GPIO0->FIODIR &= ~(KBD_COL0_MASK | KBD_COL1_MASK | KBD_COL2_MASK);

    LPC_PINCON->PINMODE7 |= ((0x02UL << 18) | (0x02UL << 20)); /* P3.25, P3.26 Tristate */
    LPC_PINCON->PINMODE3 |= (0x02UL << 10);                     /* P1.21 Tristate */
    LPC_PINCON->PINMODE1 |= ((0x02UL << 8) | (0x02UL << 10) | (0x02UL << 12)); /* P0.20..P0.22 Tristate */
}

static void Output_Row(int iRow)
{
    /* Set all row pins logic HIGH first */
    LPC_GPIO3->FIOSET = (KBD_ROW0_MASK | KBD_ROW1_MASK);
    LPC_GPIO1->FIOSET = KBD_ROW2_MASK;

    /* Assert selected row LOW */
    if (iRow == 0) {
        LPC_GPIO3->FIOCLR = KBD_ROW0_MASK; /* output_row(0): Clears P3.26 LOW */
    } else if (iRow == 1) {
        LPC_GPIO3->FIOCLR = KBD_ROW1_MASK; /* output_row(1): Clears P3.25 LOW */
    } else if (iRow == 2) {
        LPC_GPIO1->FIOCLR = KBD_ROW2_MASK; /* output_row(2): Clears P1.21 LOW */
    }
}

static int Kbd_Scan(void)
{
    int iKey_Value = 0;

    /* 1. Ensure all rows are idle HIGH before scanning */
    LPC_GPIO3->FIOSET = (KBD_ROW0_MASK | KBD_ROW1_MASK);
    LPC_GPIO1->FIOSET = KBD_ROW2_MASK;

    delay_us(500);

    /* 2. Baseline check (as in v6.1.9): with ALL rows HIGH every column must read
     * HIGH. A column already LOW is not a keypress on the row being scanned, for
     * example when the LPC845 is holding ROW2 low, so ignore that column. */
    uint32_t stuck_cols = ~LPC_GPIO0->FIOPIN;

    /* 3. Scan rows 0..2 */
    for (int iRow_No = 0; iRow_No < 3; iRow_No++) {
        Output_Row(iRow_No);
        delay_us(1000); /* Row settling delay from v6.1.9 */
        uint32_t port0 = LPC_GPIO0->FIOPIN;

        /* Restore rows to HIGH immediately after reading columns */
        LPC_GPIO3->FIOSET = (KBD_ROW0_MASK | KBD_ROW1_MASK);
        LPC_GPIO1->FIOSET = KBD_ROW2_MASK;

        /* Check columns: Col 0 = P0.22, Col 1 = P0.21, Col 2 = P0.20 */
        if ((port0 & KBD_COL0_MASK) == 0 && !(stuck_cols & KBD_COL0_MASK)) {
            iKey_Value = iRow_No * 3 + 1;
            break;
        } else if ((port0 & KBD_COL1_MASK) == 0 && !(stuck_cols & KBD_COL1_MASK)) {
            iKey_Value = iRow_No * 3 + 2;
            break;
        } else if ((port0 & KBD_COL2_MASK) == 0 && !(stuck_cols & KBD_COL2_MASK)) {
            iKey_Value = iRow_No * 3 + 3;
            break;
        }
    }

    /* Ensure rows remain HIGH */
    LPC_GPIO3->FIOSET = (KBD_ROW0_MASK | KBD_ROW1_MASK);
    LPC_GPIO1->FIOSET = KBD_ROW2_MASK;

    return iKey_Value;
}

static void vKbd_Task(void *pvParameters)
{
    (void)pvParameters;
    int iLast_Raw = 0;
    int iLatched_Key = 0;
    uint32_t debounce_count = 0;
    uint32_t hold_ticks = 0;
    bool pins_released = false;
    /* DSP may still be held from waking the unit: ignore keys until all are released,
     * otherwise that same press would open the power save prompt straight after boot. */
    bool wait_for_release = true;

    keypad_init_hw();
    vTaskDelay(50 / portTICK_RATE_MS);

    for (;;) {
        vTaskDelay(50 / portTICK_RATE_MS); /* Scan interval from v6.1.9 */

        /* Keep scanning during a scheduled wake with the LCD off: any key turns the LCD on */
        if (s_system_asleep) {
            if (!pins_released) {
                kbd_release_pins_for_sleep();
                pins_released = true;
                iLast_Raw = 0;
                iLatched_Key = 0;
                debounce_count = 0;
                hold_ticks = 0;
            }
            vTaskDelay(200 / portTICK_RATE_MS);
            continue;
        } else if (pins_released) {
            keypad_init_hw();
            pins_released = false;
            wait_for_release = true;
            vTaskDelay(50 / portTICK_RATE_MS);
        }

        /* v6.1.9 debounce: accept a key after 2 matching scans (~100ms) */
        int iRaw = Kbd_Scan();
        if (wait_for_release) {
            if (iRaw == 0) {
                wait_for_release = false;
            }
            continue;
        }
        if (iRaw) {
            if (iRaw == iLast_Raw) {
                debounce_count++;
                if (debounce_count == 2) {
                    iLatched_Key = iRaw;
                    hold_ticks = 0;
                    g_dbg_last_key = (uint32_t)iRaw;
                    kbd_fifo_put((key_code_t)iRaw);
                } else if (debounce_count > 2 && iLatched_Key == iRaw) {
                    /* Auto-repeat after 500ms, every 150ms. Arrow keys only: repeating
                     * EN could confirm an erase or a Station ID save from one long press.
                     */
                    hold_ticks++;
                    if (hold_ticks >= 10 && (hold_ticks % 3) == 0 &&
                        (iRaw == KEY_UP || iRaw == KEY_DOWN || iRaw == KEY_LEFT || iRaw == KEY_RIGHT)) {
                        kbd_fifo_put((key_code_t)iRaw);
                    }
                }
            } else {
                iLast_Raw = iRaw;
                debounce_count = 1;
            }
        } else {
            iLast_Raw = 0;
            iLatched_Key = 0;
            debounce_count = 0;
            hold_ticks = 0;
        }
    }
}

void ui_reset_activity_timer(void)
{
    s_inactivity_ticks = 0;
}

bool ui_is_operator_active(void)
{
    return s_display_active && s_operator_touched_key;
}

bool ui_is_display_active(void)
{
    return s_display_active;
}

bool ui_is_operator_touched(void)
{
    return s_operator_touched_key;
}

/* LCD lines low and the backlight off. Driven high or pulled up while the LCD's 5 V rail is off, they
 * power the backlight and controller weakly through the pins: a flicker at every slot wake (1922,
 * 2026-11-09) and wasted current. The LCD is set up once the boot is known to be an operator's. */
static void lcd_pins_quiet(void)
{
    PINSEL_CFG_Type pin_cfg;
    pin_cfg.Portnum = LCD_PORT;
    pin_cfg.Funcnum = PINSEL_FUNC_0;
    pin_cfg.Pinmode = PINSEL_PINMODE_TRISTATE;
    pin_cfg.OpenDrain = PINSEL_PINMODE_NORMAL;

    const uint8_t lcd_pins[] = {PIN_LCD_RS, PIN_LCD_RW, PIN_LCD_EN, PIN_LCD_D4, PIN_LCD_D5, PIN_LCD_D6, PIN_LCD_D7, PIN_LCD_BL};
    for (size_t i = 0; i < sizeof(lcd_pins); i++) {
        pin_cfg.Pinnum = lcd_pins[i];
        PINSEL_ConfigPin(&pin_cfg);
    }
    LPC_GPIO2->FIOCLR = (MASK_LCD_RS | MASK_LCD_RW | MASK_LCD_EN | MASK_LCD_D4 | MASK_LCD_D5 | MASK_LCD_D6 | MASK_LCD_D7 | MASK_LCD_BL);
    LPC_GPIO2->FIODIR |= (MASK_LCD_RS | MASK_LCD_RW | MASK_LCD_EN | MASK_LCD_D4 | MASK_LCD_D5 | MASK_LCD_D6 | MASK_LCD_D7 | MASK_LCD_BL);
    s_lcd_initialized = false;
}

void ui_display_init(void)
{
    keypad_init_hw();
    lcd_pins_quiet();
    s_current_screen_idx = 0;
    s_ui_mode = UI_MODE_BROWSE;
    s_status_sms_state = STATUS_UI_IDLE;
    s_status_sms_timer = 0;
    s_sms_task_handle = NULL;
    s_anim_counter = 0;
    strncpy(s_status_sms_substep, "PRESS EN TO SEND", sizeof(s_status_sms_substep) - 1);
    sim7600_set_progress_callback(on_sms_progress);
    s_display_active = true;
    s_system_asleep = false;
    s_operator_touched_key = false;
    s_force_redraw = true;
    s_cached_line1[0] = '\0';
    s_cached_line2[0] = '\0';
    ui_reset_activity_timer();

    /* Spawn dedicated Keypad Task (Priority 3, 160 words stack) */
    static xTaskHandle s_kbd_task_handle = NULL;
    if (s_kbd_task_handle == NULL) {
        xTaskCreate(vKbd_Task, (const signed char *)"KbdTask", 160, NULL, 3, &s_kbd_task_handle);
    }

    telemetry_data_t telem;
    memset(&telem, 0, sizeof(telem));
    modem_status_t mstat;
    memset(&mstat, 0, sizeof(mstat));
    mstat.csq_dbm = -113;
    render_screen(s_screens[0], &telem, &mstat);
}

bool ui_is_boot_mode_known(void)
{
    return s_boot_mode_known;
}

bool ui_was_scheduled_boot(void)
{
    return s_scheduled_boot;
}

void ui_set_slot_busy(bool busy)
{
    s_slot_busy = busy;
}

void ui_enter_sleep(void)
{
    if (s_system_asleep) {
        return;
    }

    /* Never power down over a 15-minute slot, with GPS searching, or before new firmware is confirmed */
    if (s_slot_busy || s_gps_running || s_fw_running || slot_scheduler_is_due() || fw_app_trial_unconfirmed()) {
        storage_log_debug("PWR", fw_app_trial_unconfirmed() ? "Sleep refused: new firmware not yet confirmed"
                                                            : "Sleep refused: 15-minute slot is due or running");
        s_inactivity_ticks = 0;
        return;
    }

    bool was_display_active = s_display_active;
    storage_log_debug("PWR", "Entering sleep: shutting down UI, modem, and sending $f7;");

    /* 1. Mark system and display asleep */
    s_system_asleep = true;
    s_display_active = false;
    s_operator_touched_key = false;
    s_inactivity_ticks = 0;
    s_force_redraw = true;
    s_cached_line1[0] = '\0';
    s_cached_line2[0] = '\0';

    /* 2. Purge keypad FIFO */
    s_kbd_head = 0;
    s_kbd_tail = 0;

    /* 3. Turn off cursor if in edit mode */
    if (s_ui_mode == UI_MODE_EDIT_STN_ID || s_ui_mode == UI_MODE_EDIT_TIME) {
        lcd_command(0x0C);
    }
    s_ui_mode = UI_MODE_BROWSE;

    /* 4. Display sleep banner on LCD (it has no power during a scheduled wake) */
    if (was_display_active) {
        lcd_command(0x80);
        lcd_print("POWERING DOWN...");
        lcd_command(0xC0);
        lcd_print("DSP KEY TO WAKE ");
        vTaskDelay(1200 / portTICK_RATE_MS);
    }

    /* 5. Clear LCD and turn off display & backlight */
    lcd_command(0x01);
    delay_us(2000);
    lcd_command(0x08); /* Display OFF */
    LPC_GPIO2->FIOCLR = MASK_LCD_BL; /* Backlight OFF */

    /* 6. Power off SIM7600 cellular modem completely */
    sim7600_power_off();

    /* 7. Flush any pending logs to SD card */
    storage_log_flush();

    /* 8. Release keypad pins to High-Z Tristate so LPC845 sees open circuit */
    kbd_release_pins_for_sleep();
    vTaskDelay(100 / portTICK_RATE_MS);

    /* 9. Clear cached intermcu power flags */
    intermcu_clear_power_flags();

    /* 10. Cut LCD 5V rail and send full power cut command ($f7;) to LPC845 coprocessor */
    hal_power_lcd_off();
    vTaskDelay(100 / portTICK_RATE_MS);

    /* Shutting down took a few seconds: if a slot came due meanwhile, $f7 would cut power
     * until the next slot. Restart instead, and the scheduler will send this one. */
    if (slot_scheduler_is_due()) {
        storage_log_debug("PWR", "Slot came due during shutdown: restarting instead of sleeping");
        storage_log_flush();
        NVIC_SystemReset();
    }

    storage_log_debug("PWR", "Power cut command $f7 sent to LPC845");
    storage_log_flush();
    storage_power_off_mark(); /* v6.9.31: so the log's last block isn't the card's last write */
    /* v6.9.29: a second for the card before power goes. It did not stop BIHAR's card losing the
     * log's last block (v6.9.31's mark is for that); kept while that is tested. */
    vTaskDelay(SD_SETTLE_BEFORE_POWER_OFF_MS / portTICK_RATE_MS);
    hal_power_sleep_system(); /* Sends $f7;\012 */

    /* Status frames already on their way still carry the old rail flags. Let them pass, or
     * one would look like the LPC845 waking us straight back up. */
    vTaskDelay(3000 / portTICK_RATE_MS);
    intermcu_clear_power_flags();

    /* 11. Sentry sleep loop:
     * On battery, LPC845 cuts LPC1768 power rail and system shuts down completely.
     * On bench power (debugger probe supplying 3.3V), LPC1768 sits in this low-power loop.
     * When LPC845 restores power rails ('U' or 'G') on DSP key or 15-minute slot,
     * LPC1768 triggers NVIC_SystemReset(), rebooting cleanly into active operation! */
    /* Still running 10 s after $f7 means the LPC845 missed it (a lost frame): send it again, up to
     * 3 times. Otherwise this chip would stay powered until the next slot. Harmless on bench power,
     * where the probe keeps this chip running anyway. */
    uint32_t since_f7_ms = 0;
    int f7_resends = 0;
    while (s_system_asleep) {
        vTaskDelay(200 / portTICK_RATE_MS);
        wd_checkin();   /* Waiting here is not a hang: no false "stopped responding" restart */
        if (intermcu_is_rails_active()) {
            s_system_asleep = false;
            storage_log_debug("PWR", "LPC845 awakened rails -> resetting LPC1768");
            NVIC_SystemReset();
        }
        since_f7_ms += 200;
        if (since_f7_ms >= 10000 && f7_resends < 3 && slot_scheduler_is_due()) {
            /* A slot came due while still powered: run it instead of cutting power under it */
            storage_log_debug("PWR", "Still powered after $f7 and a slot is due: restarting to run it");
            storage_log_flush();
            NVIC_SystemReset();
        }
        if (since_f7_ms >= 10000 && f7_resends < 3) {
            since_f7_ms = 0;
            f7_resends++;
            storage_log_debug("PWR", f7_resends < 3 ? "Still powered 10s after $f7: sending it again"
                                                    : "Still powered 10s after $f7: sending it a last time");
            storage_log_flush();
            storage_power_off_mark();
            vTaskDelay(SD_SETTLE_BEFORE_POWER_OFF_MS / portTICK_RATE_MS);
            hal_power_sleep_system();
        }
    }
}

/* Screen Rendering Engine */
static void render_screen(screen_type_t scr, const telemetry_data_t *telemetry, const modem_status_t *modem_stat)
{
    char line1[17] = "";
    char line2[17] = "";
    char num[8];

    if (s_ui_mode == UI_MODE_EDIT_STN_ID) {
        snprintf(line1, sizeof(line1), "SET STATION ID  ");
        snprintf(line2, sizeof(line2), "ID: %-12.12s", s_edit_stn_id + s_edit_first_pos);
    } else if (s_ui_mode == UI_MODE_EDIT_TIME) {
        snprintf(line1, sizeof(line1), "SET DATE/TIME   ");
        snprintf(line2, sizeof(line2), "%.2s-%.2s-%.4s %.2s:%.2s", s_edit_time, s_edit_time + 2,
                 s_edit_time + 4, s_edit_time + 8, s_edit_time + 10);
    } else if (s_ui_mode == UI_MODE_CALIB) {
        int tips = intermcu_get_calib_tips();
        fmt_fixed(num, sizeof(num), tips * RF_BUCKET_RESOLUTION, 4, 2);
        snprintf(line1, sizeof(line1), "Count     RF(mm)");
        snprintf(line2, sizeof(line2), "%-5d     %-6s", tips, num);
    } else if (s_ui_mode == UI_MODE_GPS_CHOICE) {
        snprintf(line1, sizeof(line1), "SEND SAVED GPS? ");
        snprintf(line2, sizeof(line2), "EN=SEND UP=NEW  ");
    } else if (s_ui_mode == UI_MODE_ERASE_CONFIRM) {
        snprintf(line1, sizeof(line1), "ARE YOU SURE?   ");
        snprintf(line2, sizeof(line2), "PRESS EN: CONFIRM");
    } else if (s_ui_mode == UI_MODE_MENU) {
        snprintf(line1, sizeof(line1), "MENU       %02u/%02u", (unsigned int)(s_menu_pos + 1), (unsigned int)NUM_MENU_ITEMS);
        snprintf(line2, sizeof(line2), ">%-15.15s", s_menu_items[s_menu_pos].label);
#if ENABLE_TEST_SEND
    } else if (s_ui_mode == UI_MODE_TEST_SEND) {
        static const char *const field_names[] = {"DAY", "MON", "YEAR", "HOUR", "MIN"};
        snprintf(line1, sizeof(line1), "%02d-%02d-%04d %02d:%02d", s_test_day, s_test_month, s_test_year,
                 s_test_slot_min / 60, s_test_slot_min % 60);
        if (s_test_running) {
            snprintf(line2, sizeof(line2), "%-16.16s", s_status_sms_substep);
        } else if (s_test_status) {
            snprintf(line2, sizeof(line2), "%-16.16s", s_test_status);
        } else {
            snprintf(line2, sizeof(line2), "SET %-4s EN=SEND", field_names[s_test_field]);
        }
#endif
    } else if (s_ui_mode == UI_MODE_FW_UPDATE) {
        if (s_fw_ui == FW_UI_MENU) {
            snprintf(line1, sizeof(line1), "FIRMWARE UPDATE ");
            snprintf(line2, sizeof(line2), ">%-15.15s", s_fw_menu[s_fw_pos]);
        } else if (s_fw_ui == FW_UI_BUSY) {
            snprintf(line1, sizeof(line1), "%-16.16s", s_fw_line1);
            snprintf(line2, sizeof(line2), "%-16.16s", s_status_sms_substep);
        } else {
            snprintf(line1, sizeof(line1), "%-16.16s", s_fw_line1);
            snprintf(line2, sizeof(line2), "%-16.16s", s_fw_line2);
        }
    } else if (s_ui_mode == UI_MODE_SLEEP_CONFIRM) {
        snprintf(line1, sizeof(line1), "POWER SAVE MODE?");
        snprintf(line2, sizeof(line2), "EN=YES  OTHER=NO");
    } else if (s_ui_mode == UI_MODE_KEY_TEST) {
        static const char *const key_names[] = {
            "", "MENU", "UP", "CALIB", "LEFT", "ENTER", "RIGHT", "BACK", "DOWN", "DSP"
        };
        snprintf(line1, sizeof(line1), "KEY TEST BACK x2");
        if (s_key_test_last == KEY_NONE) {
            snprintf(line2, sizeof(line2), "PRESS ANY KEY");
        } else {
            snprintf(line2, sizeof(line2), "%-6s K:%d", key_names[s_key_test_last], (int)s_key_test_last);
        }
    } else {
        switch (scr) {
            case SCR_STATION_ID: {
                #if (UNIT_CFG == KSNDMC_TRG)
                const char *cfg = "KSNDMC - TRG";
                #elif (UNIT_CFG == BIHAR_TRG)
                const char *cfg = "BIHAR - TRG";
                #else
                const char *cfg = "KSNDMC - TWS";
                #endif
                /* Landing page: type on row 1, station ID on row 2, both centred. Both chips'
                 * versions are on the next screen (SCR_FIRMWARE). */
                snprintf(line1, sizeof(line1), "%*s%s", (int)(16 - strlen(cfg)) / 2, "", cfg);
                char id_text[17];
                snprintf(id_text, sizeof(id_text), "ID: %s", station_id_for_display(flash_nvram_get_station_id()));
                snprintf(line2, sizeof(line2), "%*s%s", (int)(16 - strlen(id_text)) / 2, "", id_text);
                break;
            }

            case SCR_FIRMWARE: {
                /* Named as the MAIN F/W and CTRL F/W updates. The LPC845's version is read about
                 * 11 s after boot; "----" until then, or if it never answers */
                const char *v845 = fw_app_845_version();
                snprintf(line1, sizeof(line1), "MAIN  %-10.10s", FIRMWARE_VERSION);
                snprintf(line2, sizeof(line2), "CTRL  %-10.10s", (v845 && v845[0]) ? v845 : "----");
                break;
            }

            case SCR_DATETIME:
                if (strlen(telemetry->present_time) >= 14 && strncmp(telemetry->present_time, "20", 2) == 0) {
                    snprintf(line1, sizeof(line1), "DATE: %.2s/%.2s/%.4s",
                        telemetry->present_time + 6, telemetry->present_time + 4, telemetry->present_time);
                    if (intermcu_845_rtc_state() > 0) {
                        /* CTRL v1.11 keeps the time itself: its clock chip (RTC) needs repair */
                        snprintf(line2, sizeof(line2), "%.2s:%.2s:%.2s RTC BAD",
                            telemetry->present_time + 8, telemetry->present_time + 10, telemetry->present_time + 12);
                    } else {
                        snprintf(line2, sizeof(line2), "TIME: %.2s:%.2s:%.2s",
                            telemetry->present_time + 8, telemetry->present_time + 10, telemetry->present_time + 12);
                    }
                } else {
                    snprintf(line1, sizeof(line1), "DATE: NOT SYNCED");
                    snprintf(line2, sizeof(line2), "TIME: WAITING... ");
                }
                break;

            case SCR_RAINFALL:
                #if (UNIT_CFG == BIHAR_TRG)
                /* BIHAR_TRG shows both 15M and Cumulative Rainfall */
                /* Live, as on the ESP32: tips show at once, not at the next slot */
                fmt_fixed(num, sizeof(num), telemetry->rf_live_15m_mm, 4, 1);
                snprintf(line1, sizeof(line1), "RF 15M: %s mm", num);
                fmt_fixed(num, sizeof(num), telemetry->rf_cum_mm + telemetry->rf_live_15m_mm, 4, 1);
                snprintf(line2, sizeof(line2), "CUM RF: %s mm", num);
                #else
                /* KSNDMC_TRG shows ONLY Cumulative Rainfall */
                snprintf(line1, sizeof(line1), "CUM RAINFALL    ");
                fmt_fixed(num, sizeof(num), telemetry->rf_cum_mm + telemetry->rf_live_15m_mm, 4, 1);
                snprintf(line2, sizeof(line2), "    %s mm    ", num);
                #endif
                break;

            case SCR_TEMP_HUM:
                fmt_fixed(num, sizeof(num), telemetry->live_temp_c, 4, 1);
                snprintf(line1, sizeof(line1), "TEMP: %s C", num);
                fmt_fixed(num, sizeof(num), telemetry->live_humidity_pct, 4, 1);
                snprintf(line2, sizeof(line2), "HUM:  %s %%", num);
                break;

            case SCR_WIND:
                fmt_fixed(num, sizeof(num), telemetry->live_wind_speed, 4, 1);
                snprintf(line1, sizeof(line1), "WS: %s m/s", num);
                snprintf(line2, sizeof(line2), "WD: %03d DEG", telemetry->live_wind_dir_deg);
                break;

            case SCR_BATTERY:
                snprintf(line1, sizeof(line1), "BATTERY VOLTAGE ");
                fmt_fixed(num, sizeof(num), telemetry->bat1_volt, 4, 2);
                snprintf(line2, sizeof(line2), "    %s V    ", num);
                break;

            case SCR_SIGNAL:
                if (!modem_stat || !modem_stat->is_registered || modem_stat->csq_dbm <= -113) {
                    snprintf(line1, sizeof(line1), "NET: SEARCHING  ");
                    snprintf(line2, sizeof(line2), "SIG: NO NET     ");
                } else {
                    const char *car = (modem_stat->carrier[0] != '\0') ? modem_stat->carrier : "4G CELL";
                    snprintf(line1, sizeof(line1), "NET: %-11.11s", car);
                    snprintf(line2, sizeof(line2), "SIG: %4d dBm 4G ", modem_stat->csq_dbm);
                }
                break;

            case SCR_LAST_LOGGED:
                snprintf(line1, sizeof(line1), "LAST LOGGED AT  ");
                const char *last_log = flash_nvram_get_last_logged();
                snprintf(line2, sizeof(line2), "%-16.16s", (last_log && last_log[0]) ? last_log : "NO DATA YET");
                break;

            case SCR_LOCATION: {
                int32_t lat = 0, lon = 0;
                if (s_gps_running) {
                    snprintf(line1, sizeof(line1), "LOCATION        ");
                    snprintf(line2, sizeof(line2), "%-16.16s", s_status_sms_substep);
                } else if (flash_nvram_get_location(&lat, &lon)) {
                    /* Five decimals is about 1 m */
                    int32_t a = (lat < 0) ? -lat : lat;
                    int32_t o = (lon < 0) ? -lon : lon;
                    snprintf(line1, sizeof(line1), "LAT:%3ld.%05ld %c", (long)(a / 1000000), (long)((a % 1000000) / 10), (lat < 0) ? 'S' : 'N');
                    /* A result shows for 3 s, then LON again (user, 2026-11-12) */
                    if (s_gps_status && (xTaskGetTickCount() - s_gps_status_tick) < GPS_STATUS_SHOW_TICKS) {
                        snprintf(line2, sizeof(line2), "%-16.16s", s_gps_status);
                    } else {
                        snprintf(line2, sizeof(line2), "LON:%3ld.%05ld %c", (long)(o / 1000000), (long)((o % 1000000) / 10), (lon < 0) ? 'W' : 'E');
                    }
                } else {
                    snprintf(line1, sizeof(line1), "NO LOCATION YET ");
                    snprintf(line2, sizeof(line2), "%-16.16s", s_gps_status ? s_gps_status : "MENU: GPS SMS");
                }
                break;
            }

            case SCR_BACKLOG: {
                uint32_t bl = ui_get_backlog_count();
                snprintf(line1, sizeof(line1), "BACKLOG RECORDS ");
                if (bl == 0) {
                    snprintf(line2, sizeof(line2), "ALL SYNCED (00) ");
                } else {
                    snprintf(line2, sizeof(line2), "PENDING %4d PKT", (int)bl);
                }
                break;
            }

            case SCR_FIELD_CALIB:
                snprintf(line1, sizeof(line1), "FIELD CALIB     ");
                {
                    const char *calib_result = flash_nvram_get_calib_result();
                    snprintf(line2, sizeof(line2), "%-16.16s", calib_result[0] ? calib_result : "NO RESULT YET");
                }
                break;

            case SCR_SEND_STATUS:
                if (s_status_sms_state == STATUS_UI_SENDING) {
                    static const char *anim_dots[] = {
                        "SEND STATUS     ",
                        "SEND STATUS .   ",
                        "SEND STATUS ..  ",
                        "SEND STATUS ... "
                    };
                    int f = (s_anim_counter / 6) % 4;
                    snprintf(line1, sizeof(line1), "%s", anim_dots[f]);
                    snprintf(line2, sizeof(line2), "%-16.16s", s_status_sms_substep);
                } else if (s_status_sms_state == STATUS_UI_SUCCESS) {
                    snprintf(line1, sizeof(line1), "SEND STATUS SMS ");
                    snprintf(line2, sizeof(line2), "STATUS SENT OK  ");
                } else if (s_status_sms_state == STATUS_UI_FAILED) {
                    snprintf(line1, sizeof(line1), "SEND STATUS SMS ");
                    snprintf(line2, sizeof(line2), "%-16.16s", (s_status_sms_substep[0]) ? s_status_sms_substep : "STATUS FAILED   ");
                } else {
                    snprintf(line1, sizeof(line1), "SEND STATUS SMS ");
                    snprintf(line2, sizeof(line2), "PRESS EN TO SEND");
                }
                break;

            case SCR_ERASE_DATA:
                snprintf(line1, sizeof(line1), "ERASE DATA?     ");
                snprintf(line2, sizeof(line2), "PRESS EN: YES   ");
                break;

            case SCR_SLEEP:
                snprintf(line1, sizeof(line1), "POWER SAVE MODE?");
                snprintf(line2, sizeof(line2), "PRESS EN: YES   ");
                break;

            default:
                snprintf(line1, sizeof(line1), "AIO STATION");
                snprintf(line2, sizeof(line2), "ACTIVE");
                break;
        }
    }

    /* Pad lines to 16 characters */
    for (int i = strlen(line1); i < 16; i++) line1[i] = ' ';
    line1[16] = '\0';
    for (int i = strlen(line2); i < 16; i++) line2[i] = ' ';
    line2[16] = '\0';

    /* Differential rendering: skip physical LCD writes if content and cursor are unchanged */
    if (!s_force_redraw &&
        s_cached_mode == s_ui_mode &&
        s_cached_edit_pos == s_edit_pos &&
        strncmp(s_cached_line1, line1, 16) == 0 &&
        strncmp(s_cached_line2, line2, 16) == 0) {
        return;
    }

    s_force_redraw = false;
    s_cached_mode = s_ui_mode;
    s_cached_edit_pos = s_edit_pos;
    strncpy(s_cached_line1, line1, sizeof(s_cached_line1));
    strncpy(s_cached_line2, line2, sizeof(s_cached_line2));

    lcd_command(0x80);
    lcd_print(line1);

    lcd_command(0xC0);
    lcd_print(line2);

    if (s_ui_mode == UI_MODE_EDIT_STN_ID) {
        /* Line 2 format: "ID: 1922        " -> digits start at col 4 */
        lcd_set_cursor(1, 4 + s_edit_pos - s_edit_first_pos);
    } else if (s_ui_mode == UI_MODE_EDIT_TIME) {
        lcd_set_cursor(1, s_time_col[s_time_pos]);
    }
}

static bool ui_cursor_mode(void)
{
    return s_ui_mode == UI_MODE_EDIT_STN_ID || s_ui_mode == UI_MODE_EDIT_TIME;
}

/* Operator asked for power save: if a slot is running, power down once it has finished */
static void request_power_save(void)
{
    if (s_slot_busy || slot_scheduler_is_due() || fw_app_trial_unconfirmed()) {
        bool trial = fw_app_trial_unconfirmed() && !s_slot_busy && !slot_scheduler_is_due();
        storage_log_debug("PWR", trial ? "Power save deferred until the new firmware is confirmed"
                                       : "Power save deferred until 15-minute slot finishes");
        s_sleep_deferred = true;
        lcd_command(0x80);
        lcd_print(trial ? "CHECKING NEW F/W" : "SENDING DATA... ");
        lcd_command(0xC0);
        lcd_print("SLEEP WHEN DONE ");
        vTaskDelay(1500 / portTICK_RATE_MS);
        s_force_redraw = true;
    } else {
        ui_enter_sleep();
    }
}

static void calib_start(void)
{
    intermcu_calib_reset();
    hal_power_calib_start();
    s_calib_ticks = 0;
    s_ui_mode = UI_MODE_CALIB;
    s_force_redraw = true;
    storage_log_debug("CALIB", "Field calibration started");
}

static void calib_cancel(void)
{
    hal_power_calib_stop();
    s_ui_mode = UI_MODE_BROWSE;
    s_force_redraw = true;
    storage_log_debug("CALIB", "Field calibration cancelled");
}

/* Ends the run: saves "YYYY-MM-DD PASS/FAIL", shows it for 3s, stays on the calibration screen */
static void calib_finish(const telemetry_data_t *telemetry)
{
    int tips = intermcu_get_calib_tips();
    hal_power_calib_stop();

    char result[20];
    const char *pt = telemetry->present_time;
    if (strlen(pt) >= 8 && strncmp(pt, "20", 2) == 0) {
        snprintf(result, sizeof(result), "%.4s-%.2s-%.2s %s", pt, pt + 4, pt + 6, (tips == CALIB_PASS_TIPS) ? "PASS" : "FAIL");
    } else {
        snprintf(result, sizeof(result), "0000-00-00 %s", (tips == CALIB_PASS_TIPS) ? "PASS" : "FAIL");
    }
    if (!flash_nvram_set_calib_result(result)) {
        storage_log_debug("ERROR", "Could not save calibration result");
    }

    char msg[48];
    snprintf(msg, sizeof(msg), "Field calibration: %d tips, %s", tips, result);
    storage_log_debug("CALIB", msg);

    char line[17];
    lcd_command(0x80);
    lcd_print("FIELD CALIB:    ");
    snprintf(line, sizeof(line), "%-16.16s", result);
    lcd_command(0xC0);
    lcd_print(line);
    vTaskDelay(3000 / portTICK_RATE_MS);

    s_inactivity_ticks = (UI_IDLE_SEC - CALIB_READ_RESULT_SEC) * 20U;
    s_ui_mode = UI_MODE_BROWSE;
    s_force_redraw = true;
}

/* Show the power save prompt, remembering the mode to return to if the operator declines */
static void sleep_confirm_open(void)
{
    if (ui_cursor_mode()) {
        lcd_command(0x0C); /* Hide edit cursor while the prompt is showing */
    }
    s_mode_before_confirm = (s_ui_mode == UI_MODE_ERASE_CONFIRM) ? UI_MODE_BROWSE : s_ui_mode;
    s_mode_timeout_ticks = SLEEP_CONFIRM_TIMEOUT_TICKS;
    s_ui_mode = UI_MODE_SLEEP_CONFIRM;
    s_force_redraw = true;
}

static void sleep_confirm_cancel(void)
{
    s_ui_mode = s_mode_before_confirm;
    if (ui_cursor_mode()) {
        lcd_command(0x0F); /* Restore edit cursor */
    }
    s_force_redraw = true;
}

/* Jump to a screen in this build's list; does nothing if the build doesn't have it */
static void goto_screen(screen_type_t scr)
{
    for (size_t i = 0; i < NUM_SCREENS; i++) {
        if (s_screens[i] == scr) {
            s_current_screen_idx = i;
            return;
        }
    }
}

#ifdef TEST_FAULTS
static int __attribute__((noinline)) test_eat_stack(int depth)
{
    volatile char buf[128];
    buf[0] = (char)depth;
    int r = depth ? test_eat_stack(depth - 1) : 0;
    vTaskDelay(1);
    return r + buf[0];
}
#endif

static void time_edit_open(const telemetry_data_t *telemetry)
{
    char ts[15] = "20260101000000";
    if (telemetry && intermcu_ts14_believable(telemetry->present_time)) {
        memcpy(ts, telemetry->present_time, 14);
    }
    /* YYYYMMDDHHMMSS -> DDMMYYYYHHMM */
    memcpy(s_edit_time, ts + 6, 2);
    memcpy(s_edit_time + 2, ts + 4, 2);
    memcpy(s_edit_time + 4, ts, 4);
    memcpy(s_edit_time + 8, ts + 8, 4);
    s_edit_time[12] = '\0';
    s_time_pos = 0;
    s_ui_mode = UI_MODE_EDIT_TIME;
    lcd_command(0x0F);
}

static void time_edit_message(const char *line2)
{
    lcd_command(0x0C);
    lcd_command(0x80);
    lcd_print("SET DATE/TIME   ");
    lcd_command(0xC0);
    lcd_print(line2);       /* 16 characters */
    vTaskDelay(1500 / portTICK_RATE_MS);
    s_force_redraw = true;
}

/* EN on the editor */
static void time_edit_save(const telemetry_data_t *telemetry)
{
    static char ts[15], line[96], history[160];   /* UI task only */
    snprintf(ts, sizeof(ts), "%.4s%.2s%.2s%.4s00", s_edit_time + 4, s_edit_time + 2, s_edit_time, s_edit_time + 8);

    if (!intermcu_ts14_believable(ts)) {
        time_edit_message("NOT A VALID DATE");
        lcd_command(0x0F);
        return;
    }
    /* A slot running now takes its name and rain day from the clock */
    if (s_slot_busy || slot_scheduler_is_due()) {
        time_edit_message("UNIT BUSY: LATER");
        lcd_command(0x0F);
        return;
    }
    bool old_ok = intermcu_ts14_believable(telemetry->present_time);
    snprintf(line, sizeof(line), "Clock set by hand: %.14s -> %s, rain total from 0",
             old_ok ? telemetry->present_time : "(no valid time)", ts);
    uint32_t seq = clock_set_by_hand(ts);
    storage_log_debug("RTC", line);
    /* The unit's service history: kept with the debug log off too */
    snprintf(history, sizeof(history), "Station %s: %s (after record #%lu)", flash_nvram_get_station_id(),
             line, (unsigned long)seq);
    storage_append_stamped(FW_HISTORY_FILENAME, history);

    time_edit_message("TIME SET, RF 0  ");
    s_ui_mode = UI_MODE_BROWSE;
}

static void stn_edit_open(void)
{
    s_ui_mode = UI_MODE_EDIT_STN_ID;
    /* Right-aligned and padded: a stored "1922" is 001922 (KSNDMC) or 00001922 (Bihar) */
    format_station_id(s_edit_stn_id, sizeof(s_edit_stn_id), flash_nvram_get_station_id());
    /* KSNDMC: a longer real ID keeps all its digits editable. Bihar: all 8 always */
    s_edit_first_pos = (STATION_ID_DIGITS == 6 && strncmp(s_edit_stn_id, "00", 2) == 0) ? 2 : 0;
    s_edit_pos = s_edit_first_pos;
    lcd_command(0x0F); /* Turn ON blinking hardware cursor */
}

void ui_display_task_step(const telemetry_data_t *telemetry, const modem_status_t *modem_stat)
{
    if (s_system_asleep) {
        return;
    }

    /* Boot type comes from the LCD rail in the LPC845's first status frame. Its slot wake
     * leaves the LCD off; a DSP wake or first power-up has it on. */
    if (!s_boot_mode_known) {
        if (telemetry->usb_power_flag == 'u') {
            s_scheduled_boot = true;
            s_display_active = false;
            LPC_GPIO2->FIOCLR = (MASK_LCD_RS | MASK_LCD_RW | MASK_LCD_EN | MASK_LCD_D4 | MASK_LCD_D5 | MASK_LCD_D6 | MASK_LCD_D7 | MASK_LCD_BL);
            storage_log_debug("BOOT", "Boot type: SCHEDULED (LCD rail off, LCD stays off)");
            s_boot_mode_known = true;
        } else if (telemetry->usb_power_flag == 'U') {
            storage_log_debug("BOOT", "Boot type: OPERATOR (LCD rail on)");
            lcd_init_hw();
            s_boot_mode_known = true;
        } else if (++s_boot_wait_ticks >= (3 * 20)) {
            storage_log_debug("ERROR", "No status from LPC845 after 3s: assuming OPERATOR boot, check UART3 link");
            hal_power_lcd_on();
            lcd_init_hw();
            s_boot_mode_known = true;
        }
        /* Keep the UI running while waiting: the keypad must work as soon as the LCD is up */
    }

    /* Operator-requested power save, deferred while a slot was running */
    if (s_sleep_deferred && !s_slot_busy && !slot_scheduler_is_due() && !fw_app_trial_unconfirmed()) {
        s_sleep_deferred = false;
        ui_enter_sleep();
        return;
    }

    /* The LPC845's slot wake also cuts the LCD rail when someone is using the unit:
     * turn it back on rather than leave them with a dark screen. */
    if (s_display_active && telemetry->usb_power_flag == 'u') {
        if (++s_lcd_rail_off_ticks >= (5 * 20)) {
            s_lcd_rail_off_ticks = 0;
            storage_log_debug("UI", "LCD rail was off while in use: restoring it");
            hal_power_lcd_on();
            lcd_init_hw();
            if (ui_cursor_mode()) {
                lcd_command(0x0F);
            }
        }
    } else {
        s_lcd_rail_off_ticks = 0;
    }

    s_anim_counter++;

    /* An SMS should finish well inside 4 minutes. If its task is stuck it still holds the
     * modem, which would push 15-minute slots into backlog, so restart between slots. */
    static uint32_t s_sms_sending_ticks = 0;
    if (s_status_sms_state == STATUS_UI_SENDING) {
        if (++s_sms_sending_ticks >= (240 * 20) && !s_slot_busy && !slot_scheduler_is_due()) {
            storage_log_debug("ERROR", "Status SMS stuck for 4 min: restarting LPC1768");
            storage_log_flush();
            NVIC_SystemReset();
        }
    } else {
        s_sms_sending_ticks = 0;
    }

    /* Countdown timer for status SMS result message */
    if (s_status_sms_timer > 0) {
        s_status_sms_timer--;
        if (s_status_sms_timer == 0) {
            s_status_sms_state = STATUS_UI_IDLE;
        }
    }

    /* The LCD stays on for the whole run, which stops itself after 5 minutes */
    if (s_ui_mode == UI_MODE_CALIB) {
        s_inactivity_ticks = 0;
        if (++s_calib_ticks >= CALIB_TIMEOUT_TICKS) {
            calib_finish(telemetry);
            return;
        }
    }

    /* Drop keys pressed while the UI was blocked (e.g. a 3s message screen) so they
     * don't act late on the wrong screen. In v6.1.9 these presses were never scanned. */
    static portTickType s_last_step_tick = 0;
    portTickType now_tick = xTaskGetTickCount();
    if ((now_tick - s_last_step_tick) > (250 / portTICK_RATE_MS)) {
        s_kbd_tail = s_kbd_head;
    }
    s_last_step_tick = now_tick;

    key_code_t key = ui_keypad_scan();

    /* These jobs hold the LCD on until they finish, so one that never finishes must not hold it forever */
    bool job_running = s_status_sms_state == STATUS_UI_SENDING || s_gps_running
#if ENABLE_TEST_SEND
                       || s_test_running
#endif
                       ;
    static uint32_t s_job_ticks = 0;
    if (!job_running) {
        s_job_ticks = 0;
    } else if (++s_job_ticks == UI_JOB_LIMIT_SEC * 20U) {
        storage_log_debug("ERROR", "Modem job still running after 8 minutes: cancelling it");
        sim7600_request_cancel();
    } else if (s_job_ticks >= (UI_JOB_LIMIT_SEC + UI_JOB_RESET_SEC) * 20U) {
        storage_log_debug("ERROR", "Modem job did not stop after cancel: restarting");
        storage_log_flush();
        NVIC_SystemReset();
    }

    /* If display is asleep, any keypress wakes it back up */
    if (!s_display_active) {
        if (key != KEY_NONE) {
            hal_power_lcd_on();
            lcd_init_hw();
            s_current_screen_idx = 0;
            s_ui_mode = UI_MODE_BROWSE;
            s_display_active = true;
            s_operator_touched_key = true;
            s_inactivity_ticks = 0;
            s_force_redraw = true;
            storage_log_debug("UI", "Keypress detected: Display woke up from sleep");
        }
        return;
    }

    if (key != KEY_NONE) {
        s_operator_touched_key = true;
        s_inactivity_ticks = 0;
    } else if (!job_running && !s_slot_busy && !s_fw_running) {
        s_inactivity_ticks++;
        if (s_inactivity_ticks >= UI_IDLE_SEC * 20U &&
            (!sim7600_is_busy() || s_inactivity_ticks >= (UI_IDLE_SEC + UI_MODEM_FINISH_SEC) * 20U)) {
            storage_log_debug("UI", "Inactivity timeout reached -> entering auto-sleep");
            ui_enter_sleep();
            return;
        }
    }

    /* Prompts that close by themselves if the operator walks away */
    if (key == KEY_NONE && s_mode_timeout_ticks > 0 &&
        (s_ui_mode == UI_MODE_SLEEP_CONFIRM || s_ui_mode == UI_MODE_KEY_TEST || s_ui_mode == UI_MODE_GPS_CHOICE)) {
        s_mode_timeout_ticks--;
        if (s_mode_timeout_ticks == 0) {
            if (s_ui_mode == UI_MODE_SLEEP_CONFIRM) {
                sleep_confirm_cancel();
            } else {
                s_ui_mode = UI_MODE_BROWSE;
                s_force_redraw = true;
            }
        }
    }

    if (key != KEY_NONE) {
        char kmsg[32];
        snprintf(kmsg, sizeof(kmsg), "Key Evt: %d", (int)key);
        storage_log_debug("UI", kmsg);

        ui_reset_activity_timer();
        s_force_redraw = true;
        s_sleep_deferred = false; /* Operator is back: cancel any deferred power save */

        /* --- Mode: Key Test (reports every key instead of acting on it) --- */
        if (s_ui_mode == UI_MODE_KEY_TEST) {
            s_mode_timeout_ticks = KEY_TEST_TIMEOUT_TICKS;
            if (key == KEY_BACK && s_key_test_last == KEY_BACK) {
                s_ui_mode = UI_MODE_BROWSE;
            }
#ifdef TEST_FAULTS
            /* Bench test build only: forces each fault the protections must catch */
            if (key == KEY_UP) {
                storage_log_debug("TEST", "Forcing a stack overflow in the UI task");
                vTaskDelay(500 / portTICK_RATE_MS);
                test_eat_stack(14);
            } else if (key == KEY_DOWN) {
                storage_log_debug("TEST", "Forcing the UI task to stop");
                for (;;) {
                    vTaskDelay(1000 / portTICK_RATE_MS);
                }
            } else if (key == KEY_RIGHT) {
                storage_log_debug("TEST", "Forcing every task to stop (interrupts on)");
                vTaskDelay(500 / portTICK_RATE_MS);
                for (;;) {
                }
            } else if (key == KEY_LEFT) {
                extern volatile int g_test_cut_after_erase;
                storage_log_debug("TEST", "Cutting power between erasing and rewriting the settings sector");
                vTaskDelay(2000 / portTICK_RATE_MS);
                g_test_cut_after_erase = 1;
                flash_nvram_set_station_id(flash_nvram_get_station_id());
            } else if (key == KEY_EN) {
                storage_log_debug("TEST", "Forcing a hard fault (jump to 0xF0000000)");
                vTaskDelay(2000 / portTICK_RATE_MS);
                ((void (*)(void))0xF0000001UL)();
            }
#endif
            s_key_test_last = key;
            return;
        }

        #if ENABLE_TEST_SEND
        if (s_ui_mode == UI_MODE_TEST_SEND) {
            test_send_key(key);
            return;
        }
        #endif

        if (s_ui_mode == UI_MODE_FW_UPDATE) {
            fw_key(key, telemetry);
            return;
        }

        #if (UNIT_CFG == KSNDMC_TWS)
        /* CALIB is for TRG only: TWS has no rain gauge, so the key has no function */
        if (key == KEY_CALIB) {
            return;
        }
        #else
        /* Global CALIB key handler for TRG mode */
        if (key == KEY_CALIB && s_ui_mode == UI_MODE_BROWSE) {
            goto_screen(SCR_FIELD_CALIB);
            calib_start();
            return;
        }
        #endif

        /* --- Mode: Power Save Prompt (EN powers down, anything else goes back) --- */
        if (s_ui_mode == UI_MODE_SLEEP_CONFIRM) {
            if (key == KEY_EN) {
                storage_log_debug("UI", "Operator confirmed power save -> entering sleep");
                if (s_status_sms_state == STATUS_UI_SENDING) {
                    sim7600_request_cancel();
                }
                s_ui_mode = UI_MODE_BROWSE;
                request_power_save();
            } else {
                sleep_confirm_cancel();
            }
            return;
        }

        /* --- Mode: SEND GPS SMS with a saved position. EN: send it now, no search. UP: search for a
         * new one, save it, then send. Anything else goes back. --- */
        if (s_ui_mode == UI_MODE_GPS_CHOICE) {
            s_ui_mode = UI_MODE_BROWSE;
            s_force_redraw = true;
            if (key == KEY_EN) {
                gps_send_saved_start(telemetry);
            } else if (key == KEY_UP) {
                gps_fetch_start(telemetry, true);
            }
            return;
        }

        /* DSP opens the power save prompt from any screen, except during a calibration run */
        if (key == KEY_DSP && s_ui_mode != UI_MODE_CALIB) {
            sleep_confirm_open();
            return;
        }

        /* --- Mode: Field Calibration --- */
        if (s_ui_mode == UI_MODE_CALIB) {
            if (key == KEY_BACK) {
                calib_cancel();
            } else if (key == KEY_EN || key == KEY_CALIB) {
                calib_finish(telemetry);
            }
            return;
        }

        /* --- Mode: Erase Data Confirmation --- */
        if (s_ui_mode == UI_MODE_ERASE_CONFIRM) {
            if (key == KEY_EN) {
                /* Erase confirmed */
                lcd_command(0x80);
                lcd_print("ERASING DATA... ");
                lcd_command(0xC0);
                lcd_print("PLEASE WAIT...  ");

                intermcu_reset_cumulative_rain();
                storage_backlog_clear_all();
                flash_log_erase_all();
                storage_state_delete(LAST_LOGGED_FILENAME);
                flash_nvram_note_last_logged("NO DATA YET");

                vTaskDelay(1500 / portTICK_RATE_MS);
                lcd_command(0x80);
                lcd_print("DATA ERASED OK  ");
                lcd_command(0xC0);
                lcd_print("RESET COMPLETE  ");
                vTaskDelay(2000 / portTICK_RATE_MS);

                s_current_screen_idx = 0;
                s_ui_mode = UI_MODE_BROWSE;
            } else {
                /* Cancel erase on any other key */
                lcd_command(0x80);
                lcd_print("ERASE CANCELLED ");
                lcd_command(0xC0);
                lcd_print("                ");
                vTaskDelay(1000 / portTICK_RATE_MS);
                s_ui_mode = UI_MODE_BROWSE;
            }
            return;
        }

        /* --- Mode: Edit Station ID --- */
        if (s_ui_mode == UI_MODE_EDIT_STN_ID) {
            if (key == KEY_UP) {
                if (s_edit_stn_id[s_edit_pos] >= '9') {
                    s_edit_stn_id[s_edit_pos] = '0';
                } else if (s_edit_stn_id[s_edit_pos] >= '0' && s_edit_stn_id[s_edit_pos] < '9') {
                    s_edit_stn_id[s_edit_pos]++;
                } else {
                    s_edit_stn_id[s_edit_pos] = '0';
                }
            } else if (key == KEY_DOWN) {
                if (s_edit_stn_id[s_edit_pos] <= '0') {
                    s_edit_stn_id[s_edit_pos] = '9';
                } else if (s_edit_stn_id[s_edit_pos] > '0' && s_edit_stn_id[s_edit_pos] <= '9') {
                    s_edit_stn_id[s_edit_pos]--;
                } else {
                    s_edit_stn_id[s_edit_pos] = '9';
                }
            } else if (key == KEY_LEFT) {
                s_edit_pos = (s_edit_pos > s_edit_first_pos) ? (s_edit_pos - 1) : (STATION_ID_DIGITS - 1);
            } else if (key == KEY_RIGHT) {
                s_edit_pos = (s_edit_pos < STATION_ID_DIGITS - 1) ? (s_edit_pos + 1) : s_edit_first_pos;
            } else if (key == KEY_BACK) {
                /* Leave without saving */
                lcd_command(0x0C);
                s_ui_mode = UI_MODE_BROWSE;
            } else if (key == KEY_EN) {
                /* Save new Station ID to Sector 1 Flash & RAM */
                lcd_command(0x0C);

                lcd_command(0x80);
                lcd_print("STATION ID      ");
                lcd_command(0xC0);
                lcd_print("SAVING FLASH... ");

                bool ok = flash_nvram_set_station_id(s_edit_stn_id);

                lcd_command(0xC0);
                if (ok) {
                    lcd_print("SAVED OK!       ");
                } else {
                    lcd_print("SAVE FAILED!    ");
                }
                vTaskDelay(1500 / portTICK_RATE_MS);
                s_ui_mode = UI_MODE_BROWSE;
            }
            return;
        }

        /* --- Mode: Set Date/Time --- */
        if (s_ui_mode == UI_MODE_EDIT_TIME) {
            char *d = &s_edit_time[s_time_pos];
            if (key == KEY_UP) {
                *d = (*d >= '9' || *d < '0') ? '0' : (char)(*d + 1);
            } else if (key == KEY_DOWN) {
                *d = (*d <= '0' || *d > '9') ? '9' : (char)(*d - 1);
            } else if (key == KEY_LEFT) {
                s_time_pos = (s_time_pos > 0) ? (s_time_pos - 1) : 11;
            } else if (key == KEY_RIGHT) {
                s_time_pos = (s_time_pos < 11) ? (s_time_pos + 1) : 0;
            } else if (key == KEY_BACK) {
                lcd_command(0x0C);              /* Leave without setting */
                s_ui_mode = UI_MODE_BROWSE;
            } else if (key == KEY_EN) {
                time_edit_save(telemetry);
            }
            return;
        }

        /* --- Mode: MENU Shortcuts --- */
        if (s_ui_mode == UI_MODE_MENU) {
            if (key == KEY_DOWN || key == KEY_RIGHT) {
                s_menu_pos = (s_menu_pos + 1) % NUM_MENU_ITEMS;
            } else if (key == KEY_UP || key == KEY_LEFT) {
                s_menu_pos = (s_menu_pos + NUM_MENU_ITEMS - 1) % NUM_MENU_ITEMS;
            } else if (key == KEY_BACK || key == KEY_MENU) {
                s_ui_mode = UI_MODE_BROWSE;
            } else if (key == KEY_EN) {
                const menu_item_t *item = &s_menu_items[s_menu_pos];
                s_ui_mode = UI_MODE_BROWSE;
                if (item->action == MENU_GOTO_SCREEN) {
                    goto_screen(item->screen);
                } else if (item->action == MENU_SET_STATION) {
                    goto_screen(SCR_STATION_ID);
                    stn_edit_open();
                } else if (item->action == MENU_SET_TIME) {
                    goto_screen(SCR_DATETIME);
                    time_edit_open(telemetry);
                } else if (item->action == MENU_GPS) {
                    goto_screen(SCR_LOCATION);
                    if (s_gps_running) {
                        /* Already at it: the screen shows its progress */
                    } else if (flash_nvram_get_location(NULL, NULL)) {
                        s_mode_timeout_ticks = GPS_CHOICE_TIMEOUT_TICKS;
                        s_ui_mode = UI_MODE_GPS_CHOICE;
                    } else {
                        gps_fetch_start(telemetry, true);   /* No position yet: search, save, send */
                    }
                #if ENABLE_KEY_TEST
                } else if (item->action == MENU_KEY_TEST) {
                    s_key_test_last = KEY_NONE;
                    s_mode_timeout_ticks = KEY_TEST_TIMEOUT_TICKS;
                    s_ui_mode = UI_MODE_KEY_TEST;
                #endif
                } else if (item->action == MENU_FW_UPDATE) {
                    fw_open();
                #if ENABLE_TEST_SEND
                } else if (item->action == MENU_TEST_SEND) {
                    test_send_open(telemetry);
                #endif
                }
            }
            return;
        }

        /* --- Mode: Browse Mode Navigation --- */
        if (key == KEY_DOWN || key == KEY_RIGHT) {
            /* An action screen opened from MENU isn't part of the scroll: this leaves it */
            s_current_screen_idx = (s_current_screen_idx >= NUM_BROWSE_SCREENS) ? 0 : (s_current_screen_idx + 1) % NUM_BROWSE_SCREENS;
        } else if (key == KEY_UP || key == KEY_LEFT) {
            s_current_screen_idx = (s_current_screen_idx >= NUM_BROWSE_SCREENS) ? 0 : (s_current_screen_idx + NUM_BROWSE_SCREENS - 1) % NUM_BROWSE_SCREENS;
        } else if (key == KEY_MENU) {
            s_menu_pos = 0;
            s_ui_mode = UI_MODE_MENU;
        } else if (key == KEY_BACK) {
            if (s_status_sms_state == STATUS_UI_SENDING) {
                sim7600_request_cancel();
                s_status_sms_state = STATUS_UI_IDLE;
                strncpy(s_status_sms_substep, "SEND CANCELLED  ", sizeof(s_status_sms_substep) - 1);
                s_status_sms_timer = 20; /* 1.0s display */
            }
            /* From an action screen, BACK returns to the MENU it was opened from */
            if (s_current_screen_idx >= NUM_BROWSE_SCREENS) {
                s_ui_mode = UI_MODE_MENU;
            }
            s_current_screen_idx = 0;
        } else if (key == KEY_EN) {
            screen_type_t active_scr = s_screens[s_current_screen_idx];

            if (active_scr == SCR_ERASE_DATA) {
                /* Prompt Erase Confirmation */
                s_ui_mode = UI_MODE_ERASE_CONFIRM;
            } else if (active_scr == SCR_SEND_STATUS) {
                /* Transmit Status SMS Asynchronously in FreeRTOS Task (keeps UI responsive) */
                if (s_status_sms_state != STATUS_UI_SENDING && s_sms_task_handle == NULL) {
                    s_status_sms_state = STATUS_UI_SENDING;
                    strncpy(s_status_sms_substep, "MODEM POWER ON  ", sizeof(s_status_sms_substep) - 1);
                    /* 768 words: booting the modem from here is the deepest call chain in the
                     * firmware, and 512 overflowed it */
                    if (xTaskCreate(vStatusSms_Task, (const signed char *)"SmsTask", 896, NULL, 1, &s_sms_task_handle) != pdPASS) {
                        storage_log_debug("ERROR", "Not enough memory to start status SMS task");
                        s_sms_task_handle = NULL;
                        s_status_sms_state = STATUS_UI_FAILED;
                        strncpy(s_status_sms_substep, "NO MEMORY       ", sizeof(s_status_sms_substep) - 1);
                        s_status_sms_timer = 60;
                    }
                }
            } else if (active_scr == SCR_SLEEP) {
                storage_log_debug("UI", "Operator confirmed power save -> entering sleep");
                request_power_save();
                return;
            }
        }
    }

    render_screen(s_screens[s_current_screen_idx], telemetry, modem_stat);
}
