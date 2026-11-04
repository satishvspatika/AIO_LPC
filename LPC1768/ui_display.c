#include "ui_display.h"
#include "hal_power.h"
#include "user_config.h"
#include "lpc17xx_gpio.h"
#include "lpc17xx_pinsel.h"
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
/* Rows: Outputs */
#define KBD_ROW0_PORT   3
#define KBD_ROW0_PIN    25
#define KBD_ROW1_PORT   3
#define KBD_ROW1_PIN    26
#define KBD_ROW2_PORT   1
#define KBD_ROW2_PIN    21

/* Columns: Inputs with pull-ups */
#define KBD_COL0_PORT   0
#define KBD_COL0_PIN    22
#define KBD_COL1_PORT   0
#define KBD_COL1_PIN    21
#define KBD_COL2_PORT   0
#define KBD_COL2_PIN    20

static bool s_lcd_initialized = false;
static bool s_display_active = true;
static uint32_t s_activity_timestamp = 0;
static int s_current_screen = 0;

static void render_screen(int scr, const telemetry_data_t *telemetry, int csq_dbm, const char *ip_addr);

static void delay_us(uint32_t us)
{
    /* Approx calibration for 100MHz Cortex-M3 */
    volatile uint32_t count = us * 25;
    while (count--) { __asm volatile("nop"); }
}

static void lcd_pulse_en(void)
{
    delay_us(1); /* Setup time: data lines stable before EN rises */
    LPC_GPIO2->FIOSET = MASK_LCD_EN;
    delay_us(2); /* Pulse width HIGH: min 230ns */
    LPC_GPIO2->FIOCLR = MASK_LCD_EN;
    delay_us(50); /* Hold & execution recovery */
}

static void lcd_write_nibble(uint8_t nibble, bool is_data)
{
    if (is_data) {
        LPC_GPIO2->FIOSET = MASK_LCD_RS;
    } else {
        LPC_GPIO2->FIOCLR = MASK_LCD_RS;
    }
    LPC_GPIO2->FIOCLR = MASK_LCD_RW;

    /* Set or clear each data bit */
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
        delay_us(3000); /* Clear / Home requires > 1.64ms */
    } else {
        delay_us(100);  /* Standard command execution time */
    }
}

static void lcd_data(uint8_t data)
{
    lcd_write_byte(data, true);
    delay_us(50);
}

static void lcd_init_hw(void)
{
    /* Configure all LCD pins as GPIO outputs */
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
    LPC_GPIO2->FIOSET = MASK_LCD_BL; /* Turn on backlight immediately */

    /* HD44780 4-bit startup sequence */
    delay_us(60000); /* Wait > 40ms after VDD rises */
    lcd_write_nibble(0x03, false);
    delay_us(5000);  /* > 4.1 ms */
    lcd_write_nibble(0x03, false);
    delay_us(200);   /* > 100 us */
    lcd_write_nibble(0x03, false);
    delay_us(200);   /* > 100 us */
    lcd_write_nibble(0x02, false); /* Set 4-bit interface */
    delay_us(200);

    /* 4-bit mode commands matching proven Example01 sequence */
    lcd_command(0x28); /* 4-bit, 2-line, 5x8 font */
    delay_us(2000);
    lcd_command(0x08); /* Display OFF */
    delay_us(2000);
    lcd_command(0x01); /* Clear display */
    delay_us(5000);
    lcd_command(0x06); /* Entry mode: Auto-increment, no shift */
    delay_us(2000);
    lcd_command(0x0C); /* Display ON, Cursor OFF, Blink OFF */
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

/* --- Keypad Initialization & Scanning --- */
static void keypad_init_hw(void)
{
    /* Rows as GPIO Outputs */
    LPC_PINCON->PINSEL7 &= ~((0x03UL << 18) | (0x03UL << 20)); /* P3.25, P3.26 */
    LPC_PINCON->PINMODE7 &= ~((0x03UL << 18) | (0x03UL << 20)); /* Pull-Up */
    LPC_PINCON->PINMODE_OD3 &= ~((1UL << 25) | (1UL << 26));    /* Push-Pull */
    LPC_GPIO3->FIODIR |= ((1UL << 25) | (1UL << 26));
    LPC_GPIO3->FIOSET = ((1UL << 25) | (1UL << 26));            /* Idle HIGH */

    LPC_PINCON->PINSEL3 &= ~(0x03UL << 10);                     /* P1.21 */
    LPC_PINCON->PINMODE3 &= ~(0x03UL << 10);                    /* Pull-Up */
    LPC_PINCON->PINMODE_OD1 &= ~(1UL << 21);                    /* Push-Pull */
    LPC_GPIO1->FIODIR |= (1UL << 21);
    LPC_GPIO1->FIOSET = (1UL << 21);                            /* Idle HIGH */

    /* Columns as GPIO Inputs with Pull-Up */
    LPC_PINCON->PINSEL1 &= ~((0x03UL << 8) | (0x03UL << 10) | (0x03UL << 12)); /* P0.20, P0.21, P0.22 */
    LPC_PINCON->PINMODE1 &= ~((0x03UL << 8) | (0x03UL << 10) | (0x03UL << 12)); /* Internal Pull-Up */
    LPC_PINCON->PINMODE_OD0 &= ~((1UL << 20) | (1UL << 21) | (1UL << 22));      /* Normal */
    LPC_GPIO0->FIODIR &= ~((1UL << 20) | (1UL << 21) | (1UL << 22));           /* Inputs */
    LPC_GPIO0->FIOMASK &= ~((1UL << 20) | (1UL << 21) | (1UL << 22));          /* Explicitly unmasked */
}

key_code_t ui_keypad_scan(void)
{
    static key_code_t last_key = KEY_NONE;
    static uint32_t press_hold_count = 0;

    /* Ensure all rows are idle HIGH before scanning */
    LPC_GPIO3->FIOSET = ((1UL << 25) | (1UL << 26));
    LPC_GPIO1->FIOSET = (1UL << 21);

    /* Row 0 LOW */
    LPC_GPIO3->FIOCLR = (1UL << 25);
    delay_us(100);
    uint32_t cols_r0 = (LPC_GPIO0->FIOPIN >> 20) & 0x07;
    LPC_GPIO3->FIOSET = (1UL << 25);

    /* Row 1 LOW */
    LPC_GPIO3->FIOCLR = (1UL << 26);
    delay_us(100);
    uint32_t cols_r1 = (LPC_GPIO0->FIOPIN >> 20) & 0x07;
    LPC_GPIO3->FIOSET = (1UL << 26);

    /* Row 2 LOW */
    LPC_GPIO1->FIOCLR = (1UL << 21);
    delay_us(100);
    uint32_t cols_r2 = (LPC_GPIO0->FIOPIN >> 20) & 0x07;
    LPC_GPIO1->FIOSET = (1UL << 21);

    key_code_t detected = KEY_NONE;

    /* Columns: bit 0 = P0.20 (Col 2), bit 1 = P0.21 (Col 1), bit 2 = P0.22 (Col 0) */
    if (!(cols_r0 & 0x04)) detected = KEY_MENU;
    else if (!(cols_r0 & 0x02)) detected = KEY_TOP;
    else if (!(cols_r0 & 0x01)) detected = KEY_CALIB;
    else if (!(cols_r1 & 0x04)) detected = KEY_LEFT;
    else if (!(cols_r1 & 0x02)) detected = KEY_EN;
    else if (!(cols_r1 & 0x01)) detected = KEY_RIGHT;
    else if (!(cols_r2 & 0x04)) detected = KEY_BACK;
    else if (!(cols_r2 & 0x02)) detected = KEY_DOWN;
    else if (!(cols_r2 & 0x01)) detected = KEY_DSP;

    if (detected != KEY_NONE) {
        if (detected == last_key) {
            press_hold_count++;
            if (press_hold_count == 2) {
                return detected; /* Fast, responsive 100ms debounced keypress */
            }
            if (press_hold_count > 15 && (press_hold_count % 4 == 0)) {
                return detected; /* Auto-repeat */
            }
        } else {
            last_key = detected;
            press_hold_count = 1;
        }
    } else {
        last_key = KEY_NONE;
        press_hold_count = 0;
    }

    return KEY_NONE;
}

void ui_reset_activity_timer(void)
{
    s_activity_timestamp = 0; /* Reset timer */
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
    s_current_screen = 0;
    s_display_active = true;
    ui_reset_activity_timer();

    /* Render initial boot screen immediately */
    telemetry_data_t telem;
    memset(&telem, 0, sizeof(telem));
    render_screen(0, &telem, -113, "0.0.0.0");
}

/* Screen Renderers */
static void render_screen(int scr, const telemetry_data_t *telemetry, int csq_dbm, const char *ip_addr)
{
    char line1[17] = "";
    char line2[17] = "";

    switch (scr) {
        case 0: /* Landing Screen */
            #if (ACTIVE_SYSTEM == SYSTEM_TRG)
            snprintf(line1, sizeof(line1), "KSNDMC - TRG");
            #elif (ACTIVE_CONFIG == CFG_BIHAR_TRG)
            snprintf(line1, sizeof(line1), "BIHAR - TRG");
            #else
            snprintf(line1, sizeof(line1), "KSNDMC - TWS");
            #endif
            snprintf(line2, sizeof(line2), "ID:%-6s %s", STATION_ID_DEFAULT, LCD_VERSION_STRING);
            break;
        case 1: /* Date & Time */
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
        case 2: /* Rainfall */
            snprintf(line1, sizeof(line1), "RF 15M: %04.1f mm", telemetry->rf_15m_mm);
            snprintf(line2, sizeof(line2), "CUM RF: %04.1f mm", telemetry->rf_cum_mm);
            break;
        case 3: /* Temperature & Humidity (TWS Only) */
            snprintf(line1, sizeof(line1), "TEMP: %04.1f C", telemetry->temp_c);
            snprintf(line2, sizeof(line2), "HUM:  %04.1f %%", telemetry->humidity_pct);
            break;
        case 4: /* Wind Speed & Direction (TWS Only) */
            snprintf(line1, sizeof(line1), "WS: %04.1f m/s", telemetry->wind_speed_avg);
            snprintf(line2, sizeof(line2), "WD: %03d DEG", telemetry->wind_dir_deg);
            break;
        case 5: /* Battery Voltage */
            snprintf(line1, sizeof(line1), "BAT1: %04.2f V", telemetry->bat1_volt);
            snprintf(line2, sizeof(line2), "STATUS: NORMAL");
            break;
        case 6: /* Solar Status (TWS Only) */
            snprintf(line1, sizeof(line1), "SOLAR: %04.2f V", telemetry->solar_volt);
            snprintf(line2, sizeof(line2), "CHG: %s", (telemetry->solar_status == 'C') ? "CHARGING" : "STANDBY");
            break;
        case 7: /* Cellular CSQ */
            snprintf(line1, sizeof(line1), "SIGNAL: %d dBm", csq_dbm);
            snprintf(line2, sizeof(line2), "REG: 4G HOME");
            break;
        case 8: /* Modem IP Address */
            snprintf(line1, sizeof(line1), "IP ADDRESS:");
            snprintf(line2, sizeof(line2), "%-16s", (ip_addr && strlen(ip_addr)) ? ip_addr : "0.0.0.0");
            break;
        case 9: /* Sleep Display Action */
            snprintf(line1, sizeof(line1), "SLEEP SYSTEM?");
            snprintf(line2, sizeof(line2), "PRESS EN OR DSP");
            break;
        default:
            snprintf(line1, sizeof(line1), "AIO STATION");
            snprintf(line2, sizeof(line2), "ACTIVE");
            break;
    }

    lcd_command(0x80);
    lcd_print(line1);
    for (int i = strlen(line1); i < 16; i++) lcd_data(' ');

    lcd_command(0xC0);
    lcd_print(line2);
    for (int i = strlen(line2); i < 16; i++) lcd_data(' ');
}

void ui_display_task_step(const telemetry_data_t *telemetry, int csq_dbm, const char *ip_addr)
{
    key_code_t key = ui_keypad_scan();

    if (key == KEY_DSP) {
        if (s_display_active) {
            /* Blank screen and sleep LCD */
            lcd_command(0x08); /* Display OFF */
            LPC_GPIO2->FIOCLR = MASK_LCD_BL;
            hal_power_lcd_off();
            s_display_active = false;
        } else {
            /* Wake LCD */
            hal_power_lcd_on();
            lcd_init_hw();
            s_current_screen = 0;
            s_display_active = true;
            ui_reset_activity_timer();
        }
        return;
    }

    if (!s_display_active) return;

    if (key != KEY_NONE) {
        ui_reset_activity_timer();

        #if (ACTIVE_SYSTEM == SYSTEM_TRG)
        const int max_screens = 8;
        #else
        const int max_screens = 10;
        #endif

        if (key == KEY_DOWN || key == KEY_RIGHT) {
            s_current_screen = (s_current_screen + 1) % max_screens;
        } else if (key == KEY_TOP || key == KEY_LEFT) {
            /* Clamped wrap: prevent backwards jumping from Screen 0 into sleep screen */
            if (s_current_screen == 0) {
                s_current_screen = 2; /* Wrap to sensors */
            } else {
                s_current_screen--;
            }
        } else if (key == KEY_MENU) {
            s_current_screen = (s_current_screen < 5) ? 7 : 0;
        } else if (key == KEY_BACK) {
            s_current_screen = 0;
        } else if (key == KEY_EN && s_current_screen == 9) {
            /* Operator selected Sleep System */
            hal_power_sleep_system();
            s_display_active = false;
            return;
        }
    }

    render_screen(s_current_screen, telemetry, csq_dbm, ip_addr);
}

