#include "ui_display.h"
#include "hal_power.h"
#include "sim7600_driver.h"
#include "flash_nvram.h"
#include "intermcu_protocol.h"
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

/* --- 3x3 Keypad Matrix Pin Definitions --- */
/* Rows: Outputs (P3.25, P3.26, P1.21) */
#define KBD_ROW0_PORT   3
#define KBD_ROW0_PIN    25      /* Left Column:   MENU, LEFT, BACK */
#define KBD_ROW1_PORT   3
#define KBD_ROW1_PIN    26      /* Center Column: UP,   EN,   DOWN */
#define KBD_ROW2_PORT   1
#define KBD_ROW2_PIN    21      /* Right Column:  CALIB,RIGHT,DSP */

/* Columns: Inputs with pull-ups (P0.20, P0.21, P0.22) */
#define KBD_COL0_PORT   0
#define KBD_COL0_PIN    22      /* Middle Row:    LEFT, EN,   RIGHT */
#define KBD_COL1_PORT   0
#define KBD_COL1_PIN    21      /* Top Row:       MENU, UP,   CALIB */
#define KBD_COL2_PORT   0
#define KBD_COL2_PIN    20      /* Bottom Row:    BACK, DOWN, DSP */

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
#if (ACTIVE_CONFIG == CFG_KSNDMC_TWS)
static const screen_type_t s_screens[] = {
    SCR_STATION_ID,
    SCR_DATETIME,
    SCR_TEMP_HUM,
    SCR_WIND,
    SCR_BATTERY,
    SCR_SOLAR,
    SCR_SIGNAL,
    SCR_SEND_STATUS,
    SCR_ERASE_DATA,
    SCR_SLEEP
};
#elif (ACTIVE_CONFIG == CFG_KSNDMC_TRG)
static const screen_type_t s_screens[] = {
    SCR_STATION_ID,
    SCR_DATETIME,
    SCR_RAINFALL,       /* Shows ONLY Cumulative Rainfall */
    SCR_BATTERY,
    SCR_SIGNAL,
    SCR_FIELD_CALIB,
    SCR_SEND_STATUS,
    SCR_ERASE_DATA,
    SCR_SLEEP
};
#else /* CFG_BIHAR_TRG */
static const screen_type_t s_screens[] = {
    SCR_STATION_ID,
    SCR_DATETIME,
    SCR_RAINFALL,       /* Shows BOTH 15M and Cumulative Rainfall */
    SCR_BATTERY,
    SCR_SIGNAL,
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
static uint32_t s_activity_timestamp = 0;
static size_t s_current_screen_idx = 0;

static ui_mode_t s_ui_mode = UI_MODE_BROWSE;
static char s_edit_stn_id[8] = "000000";
static int s_edit_pos = 0;

/* Calibration State */
static int s_calib_tips = 0;
static uint16_t s_calib_last_raw_tips = 0;
static bool s_calib_status_pass = true;
static char s_calib_last_date[16] = "2026-01-01";

/* Status SMS Feedback */
static status_ui_state_t s_status_sms_state = STATUS_UI_IDLE;
static uint32_t s_status_sms_timer = 0;

volatile uint32_t g_dbg_last_key = 0;
volatile uint32_t g_dbg_cols_r0 = 7;
volatile uint32_t g_dbg_cols_r1 = 7;
volatile uint32_t g_dbg_cols_r2 = 7;

static void render_screen(screen_type_t scr, const telemetry_data_t *telemetry, int csq_dbm, const char *ip_addr);

static void delay_us(uint32_t us)
{
    volatile uint32_t count = us * 30;
    while (count--) { __asm volatile("nop"); }
}

static void lcd_pulse_en(void)
{
    delay_us(1);
    LPC_GPIO2->FIOSET = MASK_LCD_EN;
    delay_us(2);
    LPC_GPIO2->FIOCLR = MASK_LCD_EN;
    delay_us(50);
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
    delay_us(50);
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

/* --- 3x3 Keypad Matrix Hardware Interface --- */
static void keypad_init_hw(void)
{
    PINSEL_CFG_Type pin_cfg;
    pin_cfg.Funcnum = PINSEL_FUNC_0;
    pin_cfg.Pinmode = PINSEL_PINMODE_PULLUP;
    pin_cfg.OpenDrain = PINSEL_PINMODE_NORMAL;

    /* Rows as GPIO Outputs */
    pin_cfg.Portnum = 3; pin_cfg.Pinnum = 25; PINSEL_ConfigPin(&pin_cfg);
    pin_cfg.Portnum = 3; pin_cfg.Pinnum = 26; PINSEL_ConfigPin(&pin_cfg);
    pin_cfg.Portnum = 1; pin_cfg.Pinnum = 21; PINSEL_ConfigPin(&pin_cfg);

    /* Columns as GPIO Inputs */
    pin_cfg.Portnum = 0; pin_cfg.Pinnum = 20; PINSEL_ConfigPin(&pin_cfg);
    pin_cfg.Portnum = 0; pin_cfg.Pinnum = 21; PINSEL_ConfigPin(&pin_cfg);
    pin_cfg.Portnum = 0; pin_cfg.Pinnum = 22; PINSEL_ConfigPin(&pin_cfg);

    /* Direct register writes for maximum hardware reliability */
    LPC_PINCON->PINSEL7 &= ~((0x03UL << 18) | (0x03UL << 20)); /* P3.25, P3.26 */
    LPC_PINCON->PINMODE7 &= ~((0x03UL << 18) | (0x03UL << 20)); /* Pull-Up */
    LPC_PINCON->PINMODE_OD3 &= ~((1UL << 25) | (1UL << 26));    /* Push-Pull */
    LPC_GPIO3->FIODIR |= ((1UL << 25) | (1UL << 26));           /* Outputs */
    LPC_GPIO3->FIOMASK &= ~((1UL << 25) | (1UL << 26));         /* Unmask */
    LPC_GPIO3->FIOSET = ((1UL << 25) | (1UL << 26));             /* Idle HIGH */

    LPC_PINCON->PINSEL3 &= ~(0x03UL << 10);                     /* P1.21 */
    LPC_PINCON->PINMODE3 &= ~(0x03UL << 10);                    /* Pull-Up */
    LPC_PINCON->PINMODE_OD1 &= ~(1UL << 21);                    /* Push-Pull */
    LPC_GPIO1->FIODIR |= (1UL << 21);                           /* Output */
    LPC_GPIO1->FIOMASK &= ~(1UL << 21);                         /* Unmask */
    LPC_GPIO1->FIOSET = (1UL << 21);                            /* Idle HIGH */

    LPC_PINCON->PINSEL1 &= ~((0x03UL << 8) | (0x03UL << 10) | (0x03UL << 12)); /* P0.20, P0.21, P0.22 */
    LPC_PINCON->PINMODE1 &= ~((0x03UL << 8) | (0x03UL << 10) | (0x03UL << 12)); /* Pull-Up */
    LPC_PINCON->PINMODE_OD0 &= ~((1UL << 20) | (1UL << 21) | (1UL << 22));      /* Push-Pull */
    LPC_GPIO0->FIODIR &= ~((1UL << 20) | (1UL << 21) | (1UL << 22));           /* Inputs */
    LPC_GPIO0->FIOMASK &= ~((1UL << 20) | (1UL << 21) | (1UL << 22));          /* Explicitly unmasked */
}

static void output_row(int row)
{
    /* All rows idle HIGH */
    LPC_GPIO3->FIOSET = ((1UL << 25) | (1UL << 26));
    LPC_GPIO1->FIOSET = (1UL << 21);

    /* Assert selected row LOW */
    if (row == 0) LPC_GPIO3->FIOCLR = (1UL << 25);
    else if (row == 1) LPC_GPIO3->FIOCLR = (1UL << 26);
    else if (row == 2) LPC_GPIO1->FIOCLR = (1UL << 21);
}

/* Keypad Scanning Engine:
 * Matrix Wiring on hardware:
 * - Row 0 (P3.25): Left Column   [MENU, LEFT,  BACK]
 * - Row 1 (P3.26): Center Column [UP,   EN,    DOWN]
 * - Row 2 (P1.21): Right Column  [CALIB,RIGHT, DSP]
 *
 * - Col 1 (P0.21, bit 1): Top Row    [MENU, UP,   CALIB]
 * - Col 0 (P0.22, bit 2): Middle Row [LEFT, EN,   RIGHT]
 * - Col 2 (P0.20, bit 0): Bottom Row [BACK, DOWN, DSP]
 */
key_code_t ui_keypad_scan(void)
{
    static key_code_t last_key = KEY_NONE;
    static uint32_t press_hold_count = 0;

    /* Row 0 LOW (P3.25) -> Left physical column */
    output_row(0);
    delay_us(2000);
    uint32_t cols_r0 = (LPC_GPIO0->FIOPIN >> 20) & 0x07;

    /* Row 1 LOW (P3.26) -> Center physical column */
    output_row(1);
    delay_us(2000);
    uint32_t cols_r1 = (LPC_GPIO0->FIOPIN >> 20) & 0x07;

    /* Row 2 LOW (P1.21) -> Right physical column */
    output_row(2);
    delay_us(2000);
    uint32_t cols_r2 = (LPC_GPIO0->FIOPIN >> 20) & 0x07;

    /* All rows back to idle HIGH */
    output_row(-1);

    g_dbg_cols_r0 = cols_r0;
    g_dbg_cols_r1 = cols_r1;
    g_dbg_cols_r2 = cols_r2;

    key_code_t detected = KEY_NONE;

    /* Row 0 (Left Column): P3.25 */
    if (!(cols_r0 & 0x02))      detected = KEY_MENU;   /* Col 1 = Top */
    else if (!(cols_r0 & 0x04)) detected = KEY_LEFT;   /* Col 0 = Middle */
    else if (!(cols_r0 & 0x01)) detected = KEY_BACK;   /* Col 2 = Bottom */

    /* Row 1 (Center Column): P3.26 */
    else if (!(cols_r1 & 0x02)) detected = KEY_UP;     /* Col 1 = Top */
    else if (!(cols_r1 & 0x04)) detected = KEY_EN;     /* Col 0 = Middle */
    else if (!(cols_r1 & 0x01)) detected = KEY_DOWN;   /* Col 2 = Bottom */

    /* Row 2 (Right Column): P1.21 */
    else if (!(cols_r2 & 0x02)) detected = KEY_CALIB;  /* Col 1 = Top */
    else if (!(cols_r2 & 0x04)) detected = KEY_RIGHT;  /* Col 0 = Middle */
    else if (!(cols_r2 & 0x01)) detected = KEY_DSP;    /* Col 2 = Bottom */

    if (detected != KEY_NONE) {
        g_dbg_last_key = (uint32_t)detected;
        if (last_key == KEY_NONE) {
            last_key = detected;
            press_hold_count = 1;
            return detected;
        } else if (detected == last_key) {
            press_hold_count++;
            if (press_hold_count >= 10 && (press_hold_count % 3 == 0)) {
                return detected; /* Auto-repeat */
            }
        } else {
            last_key = detected;
            press_hold_count = 1;
            return detected;
        }
    } else {
        last_key = KEY_NONE;
        press_hold_count = 0;
    }

    return KEY_NONE;
}

void ui_reset_activity_timer(void)
{
    s_activity_timestamp = 0;
}

bool ui_is_operator_active(void)
{
    return s_display_active;
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
    s_calib_tips = 0;
    s_display_active = true;
    ui_reset_activity_timer();

    telemetry_data_t telem;
    memset(&telem, 0, sizeof(telem));
    render_screen(s_screens[0], &telem, -113, "0.0.0.0");
}

/* Screen Rendering Engine */
static void render_screen(screen_type_t scr, const telemetry_data_t *telemetry, int csq_dbm, const char *ip_addr)
{
    (void)ip_addr;
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
                #if (ACTIVE_CONFIG == CFG_KSNDMC_TRG)
                snprintf(line1, sizeof(line1), "KSNDMC - TRG");
                #elif (ACTIVE_CONFIG == CFG_BIHAR_TRG)
                snprintf(line1, sizeof(line1), "BIHAR - TRG");
                #else
                snprintf(line1, sizeof(line1), "KSNDMC - TWS");
                #endif
                snprintf(line2, sizeof(line2), "ID:%-6s %s", flash_nvram_get_station_id(), LCD_VERSION_STRING);
                break;

            case SCR_DATETIME:
                if (strlen(telemetry->present_time) >= 14) {
                    snprintf(line1, sizeof(line1), "DATE: %.2s/%.2s/%.4s",
                        telemetry->present_time + 6, telemetry->present_time + 4, telemetry->present_time);
                    snprintf(line2, sizeof(line2), "TIME: %.2s:%.2s:%.2s",
                        telemetry->present_time + 8, telemetry->present_time + 10, telemetry->present_time + 12);
                } else {
                    snprintf(line1, sizeof(line1), "DATE: --/--/----");
                    snprintf(line2, sizeof(line2), "TIME: --:--:--");
                }
                break;

            case SCR_RAINFALL:
                #if (ACTIVE_CONFIG == CFG_BIHAR_TRG)
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
                snprintf(line1, sizeof(line1), "BAT1: %04.2f V", telemetry->bat1_volt);
                snprintf(line2, sizeof(line2), "STATUS: NORMAL");
                break;

            case SCR_SOLAR:
                snprintf(line1, sizeof(line1), "SOLAR: %04.2f V", telemetry->solar_volt);
                snprintf(line2, sizeof(line2), "CHG: %s", (telemetry->solar_status == 'C') ? "CHARGING" : "STANDBY");
                break;

            case SCR_SIGNAL:
                snprintf(line1, sizeof(line1), "SIGNAL: %d dBm", csq_dbm);
                snprintf(line2, sizeof(line2), "REG: 4G HOME");
                break;

            case SCR_FIELD_CALIB:
                snprintf(line1, sizeof(line1), "FIELD CALIB     ");
                snprintf(line2, sizeof(line2), "PRESS EN OR CLB ");
                break;

            case SCR_SEND_STATUS:
                snprintf(line1, sizeof(line1), "SEND STATUS SMS");
                if (s_status_sms_state == STATUS_UI_SENDING) {
                    snprintf(line2, sizeof(line2), "SENDING STATUS..");
                } else if (s_status_sms_state == STATUS_UI_SUCCESS) {
                    snprintf(line2, sizeof(line2), "STATUS SENT OK  ");
                } else if (s_status_sms_state == STATUS_UI_FAILED) {
                    snprintf(line2, sizeof(line2), "STATUS FAILED   ");
                } else {
                    snprintf(line2, sizeof(line2), "PRESS EN TO SEND");
                }
                break;

            case SCR_ERASE_DATA:
                snprintf(line1, sizeof(line1), "ERASE DATA?     ");
                snprintf(line2, sizeof(line2), "PRESS EN: YES   ");
                break;

            case SCR_SLEEP:
                snprintf(line1, sizeof(line1), "SLEEP SYSTEM?");
                snprintf(line2, sizeof(line2), "PRESS EN OR DSP");
                break;

            default:
                snprintf(line1, sizeof(line1), "AIO STATION");
                snprintf(line2, sizeof(line2), "ACTIVE");
                break;
        }
    }

    lcd_command(0x80);
    lcd_print(line1);
    for (int i = strlen(line1); i < 16; i++) lcd_data(' ');

    lcd_command(0xC0);
    lcd_print(line2);
    for (int i = strlen(line2); i < 16; i++) lcd_data(' ');

    if (s_ui_mode == UI_MODE_EDIT_STN_ID) {
        /* Line 2 format: "ID: 001922      " -> digits start at col 4 */
        lcd_set_cursor(1, 4 + s_edit_pos);
    }
}

void ui_display_task_step(const telemetry_data_t *telemetry, int csq_dbm, const char *ip_addr)
{
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

    /* Global DSP key handler: Wake or sleep display */
    if (key == KEY_DSP) {
        if (s_display_active) {
            if (s_ui_mode == UI_MODE_EDIT_STN_ID) {
                lcd_command(0x0C);
            }
            s_ui_mode = UI_MODE_BROWSE;
            lcd_command(0x08);
            LPC_GPIO2->FIOCLR = MASK_LCD_BL;
            hal_power_lcd_off();
            s_display_active = false;
        } else {
            hal_power_lcd_on();
            lcd_init_hw();
            s_current_screen_idx = 0;
            s_ui_mode = UI_MODE_BROWSE;
            s_display_active = true;
            ui_reset_activity_timer();
        }
        return;
    }

    if (!s_display_active) return;

    if (key != KEY_NONE) {
        ui_reset_activity_timer();

        /* Global CALIB key handler */
        if (key == KEY_CALIB && s_ui_mode == UI_MODE_BROWSE) {
            #if (ACTIVE_CONFIG == CFG_KSNDMC_TWS)
            /* TWS has no rain gauge */
            lcd_command(0x80);
            lcd_print("  VARSHA - TWS  ");
            lcd_command(0xC0);
            lcd_print(" No Rain Gauge  ");
            vTaskDelay(1500 / portTICK_RATE_MS);
            #else
            /* TRG enters Field Calibration directly */
            s_ui_mode = UI_MODE_CALIB;
            s_calib_tips = 0;
            s_calib_last_raw_tips = telemetry->rf_tips_15m;
            #endif
            return;
        }

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
        if (key == KEY_DOWN || key == KEY_RIGHT) {
            s_current_screen_idx = (s_current_screen_idx + 1) % NUM_SCREENS;
        } else if (key == KEY_UP || key == KEY_LEFT) {
            s_current_screen_idx = (s_current_screen_idx + NUM_SCREENS - 1) % NUM_SCREENS;
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
                /* Transmit Status SMS */
                s_status_sms_state = STATUS_UI_SENDING;
                render_screen(active_scr, telemetry, csq_dbm, ip_addr);

                bool ok = sim7600_transmit_status_sms(telemetry, csq_dbm);
                s_status_sms_state = ok ? STATUS_UI_SUCCESS : STATUS_UI_FAILED;
                s_status_sms_timer = 60; /* 60 * 50ms = 3.0s */
            } else if (active_scr == SCR_SLEEP) {
                /* Operator selected Sleep System */
                hal_power_sleep_system();
                s_display_active = false;
                return;
            }
        }
    }

    render_screen(s_screens[s_current_screen_idx], telemetry, csq_dbm, ip_addr);
}
