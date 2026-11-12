/* Migration image: board access. Pins and timings follow the production firmware
 * (LPC1768/ui_display.c for the LCD, LPC1768/hal_power.c for UART3), copied, not shared, so
 * nothing in the production tree changes for the jig. */
#include "hal.h"
#include "LPC17xx.h"
#include "lpc17xx_pinsel.h"
#include "lpc17xx_uart.h"
#include "lpc17xx_clkpwr.h"
#include "FreeRTOS.h"
#include "task.h"

void disk_timerproc(void);   /* fat_sd/diskio.c: the SD driver's 10 ms timers */

static volatile uint32_t s_ms;

void SysTick_Handler(void)
{
    s_ms++;
    if ((s_ms % 10U) == 0U) {
        disk_timerproc();
    }
}

/* The SD driver times its waits with the RTOS tick (1 ms in this build's FreeRTOSConfig.h) */
portTickType xTaskGetTickCount(void)
{
    return (portTickType)s_ms;
}

uint32_t hal_ms(void)
{
    return s_ms;
}

void hal_delay_ms(uint32_t ms)
{
    uint32_t start = s_ms;
    while ((s_ms - start) < ms) {
    }
}

static void delay_us(uint32_t us)
{
    volatile uint32_t count = us * 30U;   /* As ui_display.c, at 100 MHz */
    while (count--) {
        __asm volatile("nop");
    }
}

void hal_init(void)
{
    SysTick_Config(SystemCoreClock / 1000U);
}

/* ---- LCD: HD44780, 4-bit, on port 2 ---- */
#define PIN_LCD_RS 4
#define PIN_LCD_RW 3
#define PIN_LCD_EN 2
#define PIN_LCD_D4 9
#define PIN_LCD_D5 6
#define PIN_LCD_D6 7
#define PIN_LCD_D7 8
#define PIN_LCD_BL 11
#define M(p) (1UL << (p))

static void lcd_nibble(uint8_t n, bool data)
{
    if (data) LPC_GPIO2->FIOSET = M(PIN_LCD_RS); else LPC_GPIO2->FIOCLR = M(PIN_LCD_RS);
    LPC_GPIO2->FIOCLR = M(PIN_LCD_RW);
    if (n & 1U) LPC_GPIO2->FIOSET = M(PIN_LCD_D4); else LPC_GPIO2->FIOCLR = M(PIN_LCD_D4);
    if (n & 2U) LPC_GPIO2->FIOSET = M(PIN_LCD_D5); else LPC_GPIO2->FIOCLR = M(PIN_LCD_D5);
    if (n & 4U) LPC_GPIO2->FIOSET = M(PIN_LCD_D6); else LPC_GPIO2->FIOCLR = M(PIN_LCD_D6);
    if (n & 8U) LPC_GPIO2->FIOSET = M(PIN_LCD_D7); else LPC_GPIO2->FIOCLR = M(PIN_LCD_D7);
    delay_us(2);
    LPC_GPIO2->FIOSET = M(PIN_LCD_EN);
    delay_us(5);
    LPC_GPIO2->FIOCLR = M(PIN_LCD_EN);
    delay_us(80);
}

static void lcd_byte(uint8_t b, bool data)
{
    lcd_nibble((uint8_t)(b >> 4), data);
    lcd_nibble((uint8_t)(b & 0x0FU), data);
    delay_us((!data && (b == 0x01 || b == 0x02)) ? 3000U : 100U);
}

void lcd_init(void)
{
    PINSEL_CFG_Type cfg;
    cfg.Portnum = 2;
    cfg.Funcnum = PINSEL_FUNC_0;
    cfg.Pinmode = PINSEL_PINMODE_PULLUP;
    cfg.OpenDrain = PINSEL_PINMODE_NORMAL;
    const uint8_t pins[] = {PIN_LCD_RS, PIN_LCD_RW, PIN_LCD_EN, PIN_LCD_D4, PIN_LCD_D5, PIN_LCD_D6, PIN_LCD_D7, PIN_LCD_BL};
    for (size_t i = 0; i < sizeof(pins); i++) {
        cfg.Pinnum = pins[i];
        PINSEL_ConfigPin(&cfg);
    }
    uint32_t all = M(PIN_LCD_RS) | M(PIN_LCD_RW) | M(PIN_LCD_EN) | M(PIN_LCD_D4) | M(PIN_LCD_D5) | M(PIN_LCD_D6) | M(PIN_LCD_D7);
    LPC_GPIO2->FIODIR |= all | M(PIN_LCD_BL);
    LPC_GPIO2->FIOCLR = all;
    LPC_GPIO2->FIOSET = M(PIN_LCD_BL);

    delay_us(60000);
    lcd_nibble(0x03, false);
    delay_us(5000);
    lcd_nibble(0x03, false);
    delay_us(200);
    lcd_nibble(0x03, false);
    delay_us(200);
    lcd_nibble(0x02, false);
    delay_us(200);
    lcd_byte(0x28, false);
    lcd_byte(0x08, false);
    lcd_byte(0x01, false);
    lcd_byte(0x06, false);
    lcd_byte(0x0C, false);
}

static void lcd_line(uint8_t addr, const char *s)
{
    lcd_byte(addr, false);
    for (int i = 0; i < 16; i++) {
        char c = (s && *s) ? *s++ : ' ';
        lcd_byte((uint8_t)c, true);
    }
}

void lcd_show(const char *line1, const char *line2)
{
    lcd_line(0x80, line1);
    lcd_line(0xC0, line2);
}

/* ---- RST_845 (P0.5) and -PGM_845 (P1.0) ---- */
static void line_pin(mig_line_t line, uint8_t *port, uint8_t *pin, volatile uint32_t **dir,
                     volatile uint32_t **set, volatile uint32_t **clr, volatile uint32_t **val)
{
    if (line == LINE_RST845) {
        *port = 0; *pin = 5;
        *dir = &LPC_GPIO0->FIODIR; *set = &LPC_GPIO0->FIOSET; *clr = &LPC_GPIO0->FIOCLR; *val = &LPC_GPIO0->FIOPIN;
    } else {
        *port = 1; *pin = 0;
        *dir = &LPC_GPIO1->FIODIR; *set = &LPC_GPIO1->FIOSET; *clr = &LPC_GPIO1->FIOCLR; *val = &LPC_GPIO1->FIOPIN;
    }
}

static void line_mode(uint8_t port, uint8_t pin, uint8_t mode)
{
    PINSEL_CFG_Type cfg;
    cfg.Portnum = port;
    cfg.Pinnum = pin;
    cfg.Funcnum = PINSEL_FUNC_0;
    cfg.Pinmode = mode;
    cfg.OpenDrain = PINSEL_PINMODE_NORMAL;
    PINSEL_ConfigPin(&cfg);
}

void line_drive(mig_line_t line, bool high)
{
    uint8_t port, pin;
    volatile uint32_t *dir, *set, *clr, *val;
    line_pin(line, &port, &pin, &dir, &set, &clr, &val);
    line_mode(port, pin, PINSEL_PINMODE_PULLUP);
    if (high) *set = 1UL << pin; else *clr = 1UL << pin;
    *dir |= 1UL << pin;
}

void line_release(mig_line_t line)
{
    uint8_t port, pin;
    volatile uint32_t *dir, *set, *clr, *val;
    line_pin(line, &port, &pin, &dir, &set, &clr, &val);
    *dir &= ~(1UL << pin);
    line_mode(port, pin, PINSEL_PINMODE_PULLUP);
}

bool line_jumper_fitted(mig_line_t line)
{
    uint8_t port, pin;
    volatile uint32_t *dir, *set, *clr, *val;
    line_pin(line, &port, &pin, &dir, &set, &clr, &val);
    *dir &= ~(1UL << pin);
    line_mode(port, pin, PINSEL_PINMODE_PULLDOWN);
    hal_delay_ms(5);
    bool high = ((*val >> pin) & 1UL) != 0;
    line_mode(port, pin, PINSEL_PINMODE_PULLUP);
    return high;
}

/* ---- UART3 ---- */
void uart3_open(uint32_t baud)
{
    PINSEL_CFG_Type pin;
    pin.Portnum = 4;
    pin.Funcnum = PINSEL_FUNC_3;
    pin.Pinmode = PINSEL_PINMODE_PULLUP;
    pin.OpenDrain = PINSEL_PINMODE_NORMAL;
    pin.Pinnum = 28;
    PINSEL_ConfigPin(&pin);
    pin.Pinnum = 29;
    PINSEL_ConfigPin(&pin);

    UART_CFG_Type cfg;
    UART_ConfigStructInit(&cfg);
    cfg.Baud_rate = baud;
    UART_Init(LPC_UART3, &cfg);
    UART_FIFO_CFG_Type fifo;
    UART_FIFOConfigStructInit(&fifo);
    UART_FIFOConfig(LPC_UART3, &fifo);
    UART_TxCmd(LPC_UART3, ENABLE);
    uart3_flush_rx();
}

void uart3_flush_rx(void)
{
    while (LPC_UART3->LSR & UART_LSR_RDR) {
        (void)LPC_UART3->RBR;
    }
}

void uart3_write(const void *data, size_t len)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++) {
        while (!(LPC_UART3->LSR & UART_LSR_THRE)) {
        }
        LPC_UART3->THR = p[i];
    }
    while (!(LPC_UART3->LSR & UART_LSR_TEMT)) {
    }
}

void uart3_puts(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    uart3_write(s, n);
}

int uart3_getc(uint32_t timeout_ms)
{
    uint32_t start = s_ms;
    do {
        if (LPC_UART3->LSR & UART_LSR_RDR) {
            return (int)(LPC_UART3->RBR & 0xFFU);
        }
    } while ((s_ms - start) < timeout_ms);
    return -1;
}

/* ---- text ---- */
char *fmt_hex(char *out, uint32_t value, int digits)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = digits - 1; i >= 0; i--) {
        out[i] = hex[value & 0xFU];
        value >>= 4;
    }
    out[digits] = '\0';
    return out + digits;
}

char *fmt_dec(char *out, uint32_t value)
{
    char tmp[11];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value);
    while (n) {
        *out++ = tmp[--n];
    }
    *out = '\0';
    return out;
}

/* fattime.c asks the production firmware for the time. No clock is read here: files get
 * 2026-01-01 00:00. */
uint32_t intermcu_get_fat_time(void)
{
    return ((uint32_t)(2026 - 1980) << 25) | (1UL << 21) | (1UL << 16);
}

/* ---- Keypad: 3 x 3 matrix, pins and scan as LPC1768/ui_display.c (keypad_init_hw, Kbd_Scan) ----
 * Rows (outputs, idle high, driven low one at a time): P3.26 (MENU UP CALIB), P3.25 (LEFT EN RIGHT).
 * Columns (inputs, pull-ups): P0.22, P0.21, P0.20. */
#define KB_ROW0 (1UL << 26)   /* P3 */
#define KB_ROW1 (1UL << 25)   /* P3 */
#define KB_COL0 (1UL << 22)   /* P0 */
#define KB_COL1 (1UL << 21)
#define KB_COL2 (1UL << 20)

void key_init(void)
{
    LPC_PINCON->PINSEL7 &= ~((0x03UL << 18) | (0x03UL << 20));   /* P3.25, P3.26: GPIO, pull-up */
    LPC_PINCON->PINMODE7 &= ~((0x03UL << 18) | (0x03UL << 20));
    LPC_PINCON->PINMODE_OD3 &= ~(KB_ROW0 | KB_ROW1);
    LPC_GPIO3->FIOMASK &= ~(KB_ROW0 | KB_ROW1);
    LPC_GPIO3->FIOSET = KB_ROW0 | KB_ROW1;
    LPC_GPIO3->FIODIR |= KB_ROW0 | KB_ROW1;
    LPC_PINCON->PINSEL1 &= ~((0x03UL << 8) | (0x03UL << 10) | (0x03UL << 12));   /* P0.20-22 */
    LPC_PINCON->PINMODE1 &= ~((0x03UL << 8) | (0x03UL << 10) | (0x03UL << 12));
    LPC_PINCON->PINMODE_OD0 &= ~(KB_COL0 | KB_COL1 | KB_COL2);
    LPC_GPIO0->FIOMASK &= ~(KB_COL0 | KB_COL1 | KB_COL2);
    LPC_GPIO0->FIODIR &= ~(KB_COL0 | KB_COL1 | KB_COL2);
}

static void rows_idle(void)
{
    LPC_GPIO3->FIOSET = KB_ROW0 | KB_ROW1;
}

/* One scan: 0 none, else row * 3 + col + 1 (the production key codes: 2 UP, 5 EN). Rows 0 and 1
 * only: row 2 (P1.21: BACK, DOWN, DSP) also goes to the CTRL board, so it is never driven here. */
static int key_scan(void)
{
    rows_idle();
    delay_us(500);
    uint32_t stuck = ~LPC_GPIO0->FIOPIN;   /* A column low with all rows high is not a key */
    for (int row = 0; row < 2; row++) {
        LPC_GPIO3->FIOCLR = (row == 0) ? KB_ROW0 : KB_ROW1;
        delay_us(1000);
        uint32_t p = LPC_GPIO0->FIOPIN;
        rows_idle();
        if (!(p & KB_COL0) && !(stuck & KB_COL0)) return row * 3 + 1;
        if (!(p & KB_COL1) && !(stuck & KB_COL1)) return row * 3 + 2;
        if (!(p & KB_COL2) && !(stuck & KB_COL2)) return row * 3 + 3;
    }
    return 0;
}

/* Waits for a press: all keys up first, then the same key on 2 scans 50 ms apart */
int key_wait(void)
{
    while (key_scan() != 0) {
        hal_delay_ms(50);
    }
    int last = 0;
    for (;;) {
        hal_delay_ms(50);
        int k = key_scan();
        if (k != 0 && k == last) return k;
        last = k;
    }
}
