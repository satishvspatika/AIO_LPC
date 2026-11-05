#include "ui_display.h"
#include "hal_power.h"
#include "sim7600_driver.h"
#include "flash_nvram.h"
#include "intermcu_protocol.h"
#include "storage_log.h"
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

/* --- 3x3 Keypad Matrix Hardware Interface (Matching Golden TWS6 kbd.c / kbd.h) ---
 * Rows (GPIO Outputs, driven LOW one at a time):
 *   Row 0: Port 3, Pin 25 (Top Row:    MENU,  UP,    CALIB)
 *   Row 1: Port 3, Pin 26 (Middle Row: LEFT,  EN,    RIGHT)
 *   Row 2: Port 1, Pin 21 (Bottom Row: BACK,  DOWN,  DSP)
 *
 * Columns (GPIO Inputs with internal pull-ups on Port 0):
 *   Col 0: Port 0, Pin 22 (Left Column:   MENU,  LEFT,  BACK)
 *   Col 1: Port 0, Pin 21 (Center Column: UP,    EN,    DOWN)
 *   Col 2: Port 0, Pin 20 (Right Column:  CALIB, RIGHT, DSP)
 */
#define KBD_ROW0_PORT   3
#define KBD_ROW0_PIN    25
#define KBD_ROW0_MASK   (1UL << 25)

#define KBD_ROW1_PORT   3
#define KBD_ROW1_PIN    26
#define KBD_ROW1_MASK   (1UL << 26)

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
    SCR_SOLAR,
    SCR_SIGNAL,
    SCR_LAST_LOGGED,
    SCR_BACKLOG,
    SCR_FIELD_CALIB,
    SCR_SEND_STATUS,
    SCR_ERASE_DATA,
    SCR_SLEEP
} screen_type_t;

/* Dynamic Carousel per System Configuration:
 * 1. KSNDMC_TWS: NO rain gauge / RF fields. Full weather sensors.
 * 2. KSNDMC_TRG: NO weather sensors. ONLY cumulative rain. Field Calib.
 * 3. BIHAR_TRG:  NO weather sensors. BOTH 15M and cumulative rain. Field Calib.
 */
#if (UNIT_CFG == KSNDMC_TWS)
static const screen_type_t s_screens[] = {
    SCR_STATION_ID,
    SCR_DATETIME,
    SCR_TEMP_HUM,
    SCR_WIND,
    SCR_BATTERY,
    SCR_SOLAR,
    SCR_SIGNAL,
    SCR_LAST_LOGGED,
    SCR_BACKLOG,
    SCR_SEND_STATUS,
    SCR_ERASE_DATA,
    SCR_SLEEP
};
#elif (UNIT_CFG == KSNDMC_TRG)
static const screen_type_t s_screens[] = {
    SCR_STATION_ID,
    SCR_DATETIME,
    SCR_RAINFALL,       /* Shows ONLY Cumulative Rainfall */
    SCR_BATTERY,
    SCR_SIGNAL,
    SCR_LAST_LOGGED,
    SCR_BACKLOG,
    SCR_FIELD_CALIB,
    SCR_SEND_STATUS,
    SCR_ERASE_DATA,
    SCR_SLEEP
};
#else /* BIHAR_TRG */
static const screen_type_t s_screens[] = {
    SCR_STATION_ID,
    SCR_DATETIME,
    SCR_RAINFALL,       /* Shows BOTH 15M and Cumulative Rainfall */
    SCR_BATTERY,
    SCR_SIGNAL,
    SCR_LAST_LOGGED,
    SCR_BACKLOG,
    SCR_FIELD_CALIB,
    SCR_SEND_STATUS,
    SCR_ERASE_DATA,
    SCR_SLEEP
};
#endif
#define NUM_SCREENS (sizeof(s_screens) / sizeof(s_screens[0]))

/* UI Operating Modes */
typedef enum {
    UI_MODE_BROWSE = 0,
    UI_MODE_EDIT_STN_ID,
    UI_MODE_CALIB,
    UI_MODE_ERASE_CONFIRM
} ui_mode_t;

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
static bool s_operator_touched_key = false;
static uint32_t s_inactivity_ticks = 0; /* 50ms ticks, 2 mins = 2400 */
static size_t s_current_screen_idx = 0;

static ui_mode_t s_ui_mode = UI_MODE_BROWSE;
static char s_edit_stn_id[8] = "000000";
static int s_edit_pos = 0;

/* Calibration State */
static int s_calib_tips = 0;
static uint16_t s_calib_last_raw_tips = 0;
static bool s_calib_status_pass = true;
static char s_calib_last_date[16] = "2026-01-01";

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

    s_sms_task_handle = NULL;
    vTaskDelete(NULL);
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
        LPC_GPIO3->FIOCLR = KBD_ROW0_MASK; /* Row 0: P3.25 */
    } else if (iRow == 1) {
        LPC_GPIO3->FIOCLR = KBD_ROW1_MASK; /* Row 1: P3.26 */
    } else if (iRow == 2) {
        LPC_GPIO1->FIOCLR = KBD_ROW2_MASK; /* Row 2: P1.21 */
    }
}

static int Kbd_Scan(void)
{
    int iKey_Value = 0;

    /* 1. Ensure all rows are idle HIGH before scanning */
    LPC_GPIO3->FIOSET = (KBD_ROW0_MASK | KBD_ROW1_MASK);
    LPC_GPIO1->FIOSET = KBD_ROW2_MASK;

    /* 2. Baseline qualification: Read columns while ALL rows are driven HIGH.
     * If all 3 column lines read 0, the front-panel 5V rail is off or unpowered.
     * Discard immediately to prevent phantom keys on power-down transitions. */
    uint32_t baseline_port0 = LPC_GPIO0->FIOPIN;
    uint32_t stuck_cols = 0;

    if ((baseline_port0 & KBD_COL0_MASK) == 0) stuck_cols |= KBD_COL0_MASK;
    if ((baseline_port0 & KBD_COL1_MASK) == 0) stuck_cols |= KBD_COL1_MASK;
    if ((baseline_port0 & KBD_COL2_MASK) == 0) stuck_cols |= KBD_COL2_MASK;

    if (stuck_cols == (KBD_COL0_MASK | KBD_COL1_MASK | KBD_COL2_MASK)) {
        return 0; /* Whole bus low: unpowered display or floating rail */
    }

    /* 3. Scan rows 0..2 */
    for (int iRow_No = 0; iRow_No < 3; iRow_No++) {
        Output_Row(iRow_No);
        vTaskDelay(1 / portTICK_RATE_MS); /* 1ms settling delay */
        uint32_t port0 = LPC_GPIO0->FIOPIN;

        /* Restore rows to HIGH immediately after reading columns */
        LPC_GPIO3->FIOSET = (KBD_ROW0_MASK | KBD_ROW1_MASK);
        LPC_GPIO1->FIOSET = KBD_ROW2_MASK;

        /* A valid key requires column to be LOW during row assert AND NOT stuck LOW at baseline */
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

enum {
    KBD_STATE_IDLE = 0,
    KBD_STATE_DEBOUNCE,
    KBD_STATE_WAIT_RELEASE
};

static void vKbd_Task(void *pvParameters)
{
    (void)pvParameters;
    int kbd_state = KBD_STATE_IDLE;
    int candidate_key = 0;
    int debounce_count = 0;
    int hold_timer = 0;
    bool pins_released = false;

    keypad_init_hw();
    vTaskDelay(300 / portTICK_RATE_MS);

    for (;;) {
        vTaskDelay(10 / portTICK_RATE_MS);

        if (!s_display_active || s_system_asleep) {
            if (!pins_released) {
                kbd_release_pins_for_sleep();
                pins_released = true;
                kbd_state = KBD_STATE_IDLE;
            }
            vTaskDelay(200 / portTICK_RATE_MS);
            continue;
        } else if (pins_released) {
            keypad_init_hw();
            pins_released = false;
            kbd_state = KBD_STATE_IDLE;
            vTaskDelay(50 / portTICK_RATE_MS);
        }

        int scanned_key = Kbd_Scan();

        switch (kbd_state) {
        case KBD_STATE_IDLE:
            if (scanned_key != 0) {
                candidate_key = scanned_key;
                debounce_count = 1;
                kbd_state = KBD_STATE_DEBOUNCE;
            }
            break;

        case KBD_STATE_DEBOUNCE:
            if (scanned_key == candidate_key) {
                debounce_count++;
                if (debounce_count >= 2) {
                    /* Confirmed stable keypress event (20ms hold): enqueue to FIFO */
                    g_dbg_last_key = (uint32_t)candidate_key;
                    kbd_fifo_put((key_code_t)candidate_key);
                    hold_timer = 0;
                    kbd_state = KBD_STATE_WAIT_RELEASE;
                }
            } else {
                /* Noise / flutter: discard */
                kbd_state = KBD_STATE_IDLE;
            }
            break;

        case KBD_STATE_WAIT_RELEASE:
            hold_timer++;
            if (scanned_key == 0) {
                /* Clean physical release confirmed */
                kbd_state = KBD_STATE_IDLE;
            } else if (scanned_key != candidate_key) {
                /* New key pressed before previous was released */
                candidate_key = scanned_key;
                debounce_count = 1;
                kbd_state = KBD_STATE_DEBOUNCE;
            } else if (hold_timer >= 40) {
                /* Stuck key recovery after 400ms hold */
                kbd_state = KBD_STATE_IDLE;
            }
            break;

        default:
            kbd_state = KBD_STATE_IDLE;
            break;
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

void ui_display_init(void)
{
    keypad_init_hw();
    hal_power_lcd_on();
    lcd_init_hw();
    s_current_screen_idx = 0;
    s_ui_mode = UI_MODE_BROWSE;
    s_status_sms_state = STATUS_UI_IDLE;
    s_status_sms_timer = 0;
    s_sms_task_handle = NULL;
    s_anim_counter = 0;
    strncpy(s_status_sms_substep, "PRESS EN TO SEND", sizeof(s_status_sms_substep) - 1);
    sim7600_set_progress_callback(on_sms_progress);
    s_calib_tips = 0;
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

void ui_enter_sleep(void)
{
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
    if (s_ui_mode == UI_MODE_EDIT_STN_ID) {
        lcd_command(0x0C);
    }
    s_ui_mode = UI_MODE_BROWSE;

    /* 4. Display sleep banner on LCD */
    lcd_command(0x80);
    lcd_print("POWERING DOWN...");
    lcd_command(0xC0);
    lcd_print("DSP KEY TO WAKE ");
    vTaskDelay(1200 / portTICK_RATE_MS);

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
    hal_power_sleep_system(); /* Sends $f7;\012 */

    /* 11. Sentry sleep loop:
     * On battery, LPC845 cuts LPC1768 power rail and system shuts down completely.
     * On bench power (debugger probe supplying 3.3V), LPC1768 sits in this low-power loop.
     * When LPC845 restores power rails ('U' or 'G') on DSP key or 15-minute slot,
     * LPC1768 triggers NVIC_SystemReset(), rebooting cleanly into active operation! */
    while (s_system_asleep) {
        vTaskDelay(200 / portTICK_RATE_MS);
        if (intermcu_is_rails_active()) {
            s_system_asleep = false;
            storage_log_debug("PWR", "LPC845 awakened rails -> resetting LPC1768");
            NVIC_SystemReset();
        }
    }
}

/* Screen Rendering Engine */
static void render_screen(screen_type_t scr, const telemetry_data_t *telemetry, const modem_status_t *modem_stat)
{
    char line1[17] = "";
    char line2[17] = "";

    if (s_ui_mode == UI_MODE_EDIT_STN_ID) {
        snprintf(line1, sizeof(line1), "SET STATION ID  ");
        snprintf(line2, sizeof(line2), "ID: %-6.6s      ", s_edit_stn_id);
    } else if (s_ui_mode == UI_MODE_CALIB) {
        float rf_mm = s_calib_tips * RF_BUCKET_RESOLUTION;
        snprintf(line1, sizeof(line1), "CALIB: TIPS   RF");
        snprintf(line2, sizeof(line2), "Count:%-3d %3d.%02dmm", s_calib_tips, (int)rf_mm, (int)((rf_mm - (int)rf_mm) * 100));
    } else if (s_ui_mode == UI_MODE_ERASE_CONFIRM) {
        snprintf(line1, sizeof(line1), "ARE YOU SURE?   ");
        snprintf(line2, sizeof(line2), "PRESS EN: CONFIRM");
    } else {
        switch (scr) {
            case SCR_STATION_ID:
                #if (UNIT_CFG == KSNDMC_TRG)
                snprintf(line1, sizeof(line1), "KSNDMC - TRG");
                #elif (UNIT_CFG == BIHAR_TRG)
                snprintf(line1, sizeof(line1), "BIHAR - TRG");
                #else
                snprintf(line1, sizeof(line1), "KSNDMC - TWS");
                #endif
                snprintf(line2, sizeof(line2), "ID:%-6s %s", flash_nvram_get_station_id(), FIRMWARE_VERSION);
                break;

            case SCR_DATETIME:
                if (strlen(telemetry->present_time) >= 14 && strncmp(telemetry->present_time, "20", 2) == 0) {
                    snprintf(line1, sizeof(line1), "DATE: %.2s/%.2s/%.4s",
                        telemetry->present_time + 6, telemetry->present_time + 4, telemetry->present_time);
                    snprintf(line2, sizeof(line2), "TIME: %.2s:%.2s:%.2s",
                        telemetry->present_time + 8, telemetry->present_time + 10, telemetry->present_time + 12);
                } else {
                    snprintf(line1, sizeof(line1), "DATE: NOT SYNCED");
                    snprintf(line2, sizeof(line2), "TIME: WAITING... ");
                }
                break;

            case SCR_RAINFALL:
                #if (UNIT_CFG == BIHAR_TRG)
                /* BIHAR_TRG shows both 15M and Cumulative Rainfall */
                snprintf(line1, sizeof(line1), "RF 15M: %04.1f mm", telemetry->rf_15m_mm);
                snprintf(line2, sizeof(line2), "CUM RF: %04.1f mm", telemetry->rf_cum_mm);
                #else
                /* KSNDMC_TRG shows ONLY Cumulative Rainfall */
                snprintf(line1, sizeof(line1), "CUM RAINFALL    ");
                snprintf(line2, sizeof(line2), "    %04.1f mm    ", telemetry->rf_cum_mm);
                #endif
                break;

            case SCR_TEMP_HUM:
                snprintf(line1, sizeof(line1), "TEMP: %04.1f C", telemetry->temp_c);
                snprintf(line2, sizeof(line2), "HUM:  %04.1f %%", telemetry->humidity_pct);
                break;

            case SCR_WIND:
                snprintf(line1, sizeof(line1), "WS: %04.1f m/s", telemetry->wind_speed_avg);
                snprintf(line2, sizeof(line2), "WD: %03d DEG", telemetry->wind_dir_deg);
                break;

            case SCR_BATTERY:
                snprintf(line1, sizeof(line1), "BATTERY VOLTAGE ");
                snprintf(line2, sizeof(line2), "    %04.2f V    ", telemetry->bat1_volt);
                break;

            case SCR_SOLAR:
                snprintf(line1, sizeof(line1), "SOLAR VOLTAGE   ");
                if (telemetry->solar_volt > 1.0f) {
                    snprintf(line2, sizeof(line2), "%04.2fV %s",
                             telemetry->solar_volt,
                             (telemetry->solar_status == 'C' || telemetry->solar_status == 'Y') ? "CHARGING" : "STANDBY");
                } else {
                    snprintf(line2, sizeof(line2), "    STANDBY     ");
                }
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

            case SCR_BACKLOG: {
                uint32_t bl = ui_get_backlog_count();
                snprintf(line1, sizeof(line1), "BACKLOG RECORDS ");
                if (bl == 0) {
                    snprintf(line2, sizeof(line2), "ALL SYNCED (00) ");
                } else {
                    snprintf(line2, sizeof(line2), "PENDING: %-4d PKTS", (int)bl);
                }
                break;
            }

            case SCR_FIELD_CALIB:
                snprintf(line1, sizeof(line1), "FIELD CALIB     ");
                snprintf(line2, sizeof(line2), "PRESS EN OR CLB ");
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
                snprintf(line1, sizeof(line1), "SLEEP SYSTEM?   ");
                snprintf(line2, sizeof(line2), "PRESS EN: SLEEP ");
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
        /* Line 2 format: "ID: 001922      " -> digits start at col 4 */
        lcd_set_cursor(1, 4 + s_edit_pos);
    }
}

void ui_display_task_step(const telemetry_data_t *telemetry, const modem_status_t *modem_stat)
{
    s_anim_counter++;

    /* Countdown timer for status SMS result message */
    if (s_status_sms_timer > 0) {
        s_status_sms_timer--;
        if (s_status_sms_timer == 0) {
            s_status_sms_state = STATUS_UI_IDLE;
        }
    }

    /* In Calibration Mode, sync tipping bucket count from coprocessor */
    if (s_ui_mode == UI_MODE_CALIB) {
        if (telemetry->rf_tips_15m != s_calib_last_raw_tips) {
            uint16_t diff = telemetry->rf_tips_15m - s_calib_last_raw_tips;
            s_calib_tips += diff;
            s_calib_last_raw_tips = telemetry->rf_tips_15m;
        }
    }

    key_code_t key = ui_keypad_scan();

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

    /* Auto-sleep after 2 minutes of inactivity (120s = 2400 ticks of 50ms) */
    if (key != KEY_NONE) {
        s_operator_touched_key = true;
        s_inactivity_ticks = 0;
    } else if (s_status_sms_state != STATUS_UI_SENDING) {
        s_inactivity_ticks++;
        if (s_inactivity_ticks >= (120 * 20)) {
            storage_log_debug("UI", "2-minute inactivity timeout reached -> entering auto-sleep");
            ui_enter_sleep();
            return;
        }
    }

    /* Global DSP key handler: Sleep display on demand */
    if (key == KEY_DSP) {
        storage_log_debug("UI", "Operator pressed DSP key -> entering sleep");
        ui_enter_sleep();
        return;
    }

    if (key != KEY_NONE) {
        ui_reset_activity_timer();
        s_force_redraw = true;

        #if (UNIT_CFG == KSNDMC_TWS)
        if (key == KEY_CALIB) {
            /* TWS Total Weather Station has no rain gauge.
             * Completely ignore CALIB key, matching golden TWS5 code line 80. */
            return;
        }
        #else
        /* Global CALIB key handler for TRG mode */
        if (key == KEY_CALIB && s_ui_mode == UI_MODE_BROWSE) {
            s_ui_mode = UI_MODE_CALIB;
            s_calib_tips = 0;
            s_calib_last_raw_tips = telemetry->rf_tips_15m;
            return;
        }
        #endif

        /* --- Mode: Field Calibration --- */
        if (s_ui_mode == UI_MODE_CALIB) {
            if (key == KEY_UP) {
                /* Operator manual bench test tip */
                s_calib_tips++;
            } else if (key == KEY_BACK) {
                /* Cancel calibration */
                lcd_command(0x80);
                lcd_print("Calibration     ");
                lcd_command(0xC0);
                lcd_print("CANCELLED       ");
                vTaskDelay(1500 / portTICK_RATE_MS);
                s_ui_mode = UI_MODE_BROWSE;
            } else if (key == KEY_EN) {
                /* Confirm calibration test */
                s_calib_status_pass = (s_calib_tips == 10);
                if (strlen(telemetry->present_time) >= 8) {
                    snprintf(s_calib_last_date, sizeof(s_calib_last_date), "%.4s-%.2s-%.2s",
                             telemetry->present_time, telemetry->present_time + 4, telemetry->present_time + 6);
                }

                lcd_command(0x80);
                lcd_print("CALIB RESULT:   ");
                lcd_command(0xC0);
                if (s_calib_status_pass) {
                    lcd_print("PASS (10 tips)  ");
                } else {
                    lcd_print("FAIL (<>10 tips)");
                }
                vTaskDelay(3000 / portTICK_RATE_MS);
                s_ui_mode = UI_MODE_BROWSE;
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
                s_edit_pos = (s_edit_pos > 0) ? (s_edit_pos - 1) : 5;
            } else if (key == KEY_RIGHT) {
                s_edit_pos = (s_edit_pos < 5) ? (s_edit_pos + 1) : 0;
            } else if (key == KEY_BACK) {
                /* Clear buffer: if any non-zero, clear all to '0' */
                bool all_zero = true;
                for (int i = 0; i < 6; i++) {
                    if (s_edit_stn_id[i] != '0') {
                        all_zero = false;
                        break;
                    }
                }
                if (!all_zero) {
                    memset(s_edit_stn_id, '0', 6);
                    s_edit_pos = 0;
                } else {
                    /* If already all zeros, cancel edit mode */
                    lcd_command(0x0C);
                    s_ui_mode = UI_MODE_BROWSE;
                }
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

        /* --- Mode: Browse Mode Navigation --- */
        if (key == KEY_DOWN) {
            s_current_screen_idx = (s_current_screen_idx + 1) % NUM_SCREENS;
        } else if (key == KEY_UP) {
            s_current_screen_idx = (s_current_screen_idx + NUM_SCREENS - 1) % NUM_SCREENS;
        } else if (key == KEY_RIGHT || key == KEY_LEFT) {
            /* Left and Right keys are dedicated to cursor navigation in Edit mode; strictly NO-OP in browse */
        } else if (key == KEY_MENU) {
            /* Quick jump between Station ID (0) and Send Status */
            if (s_current_screen_idx == 0) {
                for (size_t i = 0; i < NUM_SCREENS; i++) {
                    if (s_screens[i] == SCR_SEND_STATUS) {
                        s_current_screen_idx = i;
                        break;
                    }
                }
            } else {
                s_current_screen_idx = 0;
            }
        } else if (key == KEY_BACK) {
            if (s_status_sms_state == STATUS_UI_SENDING) {
                sim7600_request_cancel();
                s_status_sms_state = STATUS_UI_IDLE;
                strncpy(s_status_sms_substep, "SEND CANCELLED  ", sizeof(s_status_sms_substep) - 1);
                s_status_sms_timer = 20; /* 1.0s display */
            }
            s_current_screen_idx = 0;
        } else if (key == KEY_EN) {
            screen_type_t active_scr = s_screens[s_current_screen_idx];

            if (active_scr == SCR_STATION_ID) {
                /* Enter Station ID Edit Mode */
                s_ui_mode = UI_MODE_EDIT_STN_ID;
                const char *cur = flash_nvram_get_station_id();
                memset(s_edit_stn_id, '0', 6);
                s_edit_stn_id[6] = '\0';
                if (cur) {
                    size_t len = strlen(cur);
                    if (len > 6) len = 6;
                    for (size_t i = 0; i < len; i++) {
                        if (cur[i] >= '0' && cur[i] <= '9') {
                            s_edit_stn_id[i] = cur[i];
                        }
                    }
                }
                s_edit_pos = 0;
                lcd_command(0x0F); /* Turn ON blinking hardware cursor */
            } else if (active_scr == SCR_FIELD_CALIB) {
                /* Enter Field Calibration */
                s_ui_mode = UI_MODE_CALIB;
                s_calib_tips = 0;
                s_calib_last_raw_tips = telemetry->rf_tips_15m;
            } else if (active_scr == SCR_ERASE_DATA) {
                /* Prompt Erase Confirmation */
                s_ui_mode = UI_MODE_ERASE_CONFIRM;
            } else if (active_scr == SCR_SEND_STATUS) {
                /* Transmit Status SMS Asynchronously in FreeRTOS Task (keeps UI responsive) */
                if (s_status_sms_state != STATUS_UI_SENDING && s_sms_task_handle == NULL) {
                    s_status_sms_state = STATUS_UI_SENDING;
                    strncpy(s_status_sms_substep, "MODEM POWER ON  ", sizeof(s_status_sms_substep) - 1);
                    xTaskCreate(vStatusSms_Task, (const signed char *)"SmsTask", 512, NULL, 1, &s_sms_task_handle);
                }
            } else if (active_scr == SCR_SLEEP) {
                /* Operator selected Sleep System */
                storage_log_debug("UI", "Operator confirmed SLEEP SYSTEM? -> entering sleep");
                lcd_command(0x80);
                lcd_print("SYSTEM SLEEPING ");
                lcd_command(0xC0);
                lcd_print("POWERING DOWN...");
                vTaskDelay(1200 / portTICK_RATE_MS);

                ui_enter_sleep();
                return;
            }
        }
    }

    render_screen(s_screens[s_current_screen_idx], telemetry, modem_stat);
}
