/* CTRL bootloader + firmware onto an LPC845 that has no bootloader, through its ROM loader
 * (ctrl_isp.h). The ROM loader and programming code is TEST_JIG/migrate isp845.c and ctrl845.c
 * (MIG-1.4, proven on old unit 2114), with these changes for running inside the production image:
 *   - Runs before the RTOS starts, interrupts off, everything polled. Time comes from the DWT cycle
 *     counter (SysTick belongs to FreeRTOS); the SD driver's own waits are bounded by retry counts.
 *   - The CTRL bootloader (2.2 KB) is built in; the CTRL firmware comes from ctrl.hex on the card,
 *     read and checked by the production reader (fw_ctrl_check / fw_ctrl_stream), as for a normal
 *     CTRL update. Its CRC must equal the one the operator saw in the menu.
 *   - No copy of the old CTRL flash is kept (a replaced board's old firmware is of no use).
 *
 * LPC845 flash after a good run, the same as LPC845_TWS.hex from the MCU-Link: CTRL bootloader at 0,
 * firmware at 0x2400, everything else blank (so its update journal starts empty).
 * Order, so a power cut anywhere leaves a unit that can run this again:
 *   1. Sector 0 <- rescue program (rescue845.s): at power-up it keeps the rails on and opens the ROM
 *      loader, so the LPC1768 still comes up and the operator can run CTRL F/W again.
 *   2. Sectors 1-63: bootloader tail, firmware, blanks; each read back.
 *   3. Sector 0 <- the CTRL bootloader's head, read back. Then the whole chip is compared once more. */
#include "ctrl_isp.h"
#include "ctrl_isp_core.h"
#include "ctrl_isp_img.h"   /* build.sh: g_stub845[], g_rescue845[], g_ctrl_boot[] and lengths */
#include "fw_update.h"
#include "storage_log.h"
#include "flash_nvram.h"
#include "user_config.h"
#include "boot/fw_layout.h"
#include "LPC17xx.h"
#include "system_LPC17xx.h"
#include "lpc17xx_pinsel.h"
#include "lpc17xx_uart.h"
#include <stdio.h>
#include <string.h>

#define ISP_REQ_MAGIC   0x43495351UL    /* 'CISQ': run at the next boot; GPREG1 = ctrl.hex CRC */
#define ISP_DONE_MAGIC  0x43495344UL    /* 'CISD': GPREG1 = result, GPREG2 = sector, to log */


static const char *const s_result_text[ISP_RESULT_COUNT] = {
    "CTRL bootloader and firmware put on, whole chip read back",
    "JP1/JP2 not fitted: nothing done",
    "ctrl.hex missing or damaged: nothing done",
    "ctrl.hex changed since the menu check: nothing done",
    "CTRL chip ROM loader did not answer (JP1/JP2?): nothing written",
    "CTRL chip ROM loader refused unlock: nothing written",
    "CTRL chip sector 0 did not write (rescue left if possible): run CTRL F/W again",
    "CTRL chip write failed (rescue sector 0 in place): run CTRL F/W again",
    "CTRL chip read-back differs: run CTRL F/W again",
};
static const char *const s_result_lcd[ISP_RESULT_COUNT] = {
    "CTRL BOOT OK", "JP1+JP2 NOT ON", "CTRL.HEX BAD", "CTRL.HEX CHANGED", "NO ROM LOADER",
    "UNLOCK REFUSED", "SECTOR 0 FAILED", "WRITE FAILED", "CHECK FAILED",
};

/* ---- time: DWT cycle counter, as SysTick is the RTOS tick ---- */
#define DEMCR       (*(volatile uint32_t *)0xE000EDFCUL)
#define DWT_CTRL    (*(volatile uint32_t *)0xE0001000UL)
#define DWT_CYCCNT  (*(volatile uint32_t *)0xE0001004UL)

static uint32_t s_ms, s_cyc_last, s_cyc_rem, s_wdt_ms;

static void wdt_feed(void)
{
    /* The bootloader starts the watchdog on a trial boot; feeding a stopped one does nothing */
    LPC_WDT->WDFEED = 0xAA;
    LPC_WDT->WDFEED = 0x55;
}

/* Must be called at least every 40 s (it is called all the time) and before each clock change */
static uint32_t hal_ms(void)
{
    uint32_t now = DWT_CYCCNT;
    s_cyc_rem += now - s_cyc_last;
    s_cyc_last = now;
    uint32_t per_ms = SystemCoreClock / 1000U;
    s_ms += s_cyc_rem / per_ms;
    s_cyc_rem %= per_ms;
    if ((s_ms - s_wdt_ms) >= 200U) {
        s_wdt_ms = s_ms;
        wdt_feed();
    }
    return s_ms;
}

static void hal_delay_ms(uint32_t ms)
{
    uint32_t start = hal_ms();
    while ((hal_ms() - start) < ms) {
    }
}

static void timer_start(void)
{
    DEMCR |= (1UL << 24);      /* TRCENA */
    DWT_CYCCNT = 0;
    DWT_CTRL |= 1UL;           /* CYCCNTENA */
    s_cyc_last = DWT_CYCCNT;
    s_cyc_rem = 0;
    s_ms = 0;
    s_wdt_ms = 0;
}

static void delay_us(uint32_t us)
{
    volatile uint32_t count = us * 30U;   /* As ui_display.c, at 100 MHz (longer when slower) */
    while (count--) {
        __asm volatile("nop");
    }
}

/* ---- LCD: HD44780, 4-bit, on port 2 (pins as ui_display.c) ---- */
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

static void lcd_init(void)
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

static void lcd_show(const char *line1, const char *line2)
{
    lcd_line(0x80, line1);
    lcd_line(0xC0, line2);
}

/* ---- RST_845 (P0.5) and -PGM_845 (P1.0) ---- */
typedef enum { LINE_RST845, LINE_PGM845 } isp_line_t;

static void line_regs(isp_line_t line, uint8_t *port, uint8_t *pin, LPC_GPIO_TypeDef **gpio)
{
    if (line == LINE_RST845) {
        *port = 0; *pin = 5; *gpio = LPC_GPIO0;
    } else {
        *port = 1; *pin = 0; *gpio = LPC_GPIO1;
    }
}

/* PINSEL/PINMODE registers are shared with other pins and written read-modify-write; the jumper
 * check also runs from the UI task, so each write is made with interrupts off (a few us only) */
static void line_mode(uint8_t port, uint8_t pin, uint8_t mode)
{
    PINSEL_CFG_Type cfg;
    cfg.Portnum = port;
    cfg.Pinnum = pin;
    cfg.Funcnum = PINSEL_FUNC_0;
    cfg.Pinmode = mode;
    cfg.OpenDrain = PINSEL_PINMODE_NORMAL;
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    PINSEL_ConfigPin(&cfg);
    __set_PRIMASK(primask);
}

static void line_drive(isp_line_t line, bool high)
{
    uint8_t port, pin;
    LPC_GPIO_TypeDef *g;
    line_regs(line, &port, &pin, &g);
    line_mode(port, pin, PINSEL_PINMODE_PULLUP);
    if (high) g->FIOSET = 1UL << pin; else g->FIOCLR = 1UL << pin;
    g->FIODIR |= 1UL << pin;
}

/* Input with pull-up, as the production firmware otherwise leaves them */
static void line_release(isp_line_t line)
{
    uint8_t port, pin;
    LPC_GPIO_TypeDef *g;
    line_regs(line, &port, &pin, &g);
    g->FIODIR &= ~(1UL << pin);
    line_mode(port, pin, PINSEL_PINMODE_PULLUP);
}

static bool line_jumper_fitted(isp_line_t line)
{
    uint8_t port, pin;
    LPC_GPIO_TypeDef *g;
    line_regs(line, &port, &pin, &g);
    g->FIODIR &= ~(1UL << pin);     /* Nothing else drives P0.5 or P1.0 (checked) */
    line_mode(port, pin, PINSEL_PINMODE_PULLDOWN);
    for (volatile uint32_t i = 0; i < 150000U; i++) {   /* About 5 ms at 100 MHz */
    }
    bool high = ((g->FIOPIN >> pin) & 1UL) != 0;
    line_mode(port, pin, PINSEL_PINMODE_PULLUP);
    return high;
}

bool ctrl_isp_jumpers_fitted(void)
{
    return line_jumper_fitted(LINE_RST845) && line_jumper_fitted(LINE_PGM845);
}

/* ---- UART3 (P4.28/P4.29), polled ---- */
static void uart3_flush_rx(void)
{
    while (LPC_UART3->LSR & UART_LSR_RDR) {
        (void)LPC_UART3->RBR;
    }
}

static void uart3_open(uint32_t baud)
{
    UART_CFG_Type cfg;
    UART_ConfigStructInit(&cfg);
    cfg.Baud_rate = baud;
    UART_Init(LPC_UART3, &cfg);
    UART_FIFO_CFG_Type fifo;
    UART_FIFOConfigStructInit(&fifo);
    UART_FIFOConfig(LPC_UART3, &fifo);
    UART_IntConfig(LPC_UART3, UART_INTCFG_RBR, DISABLE);
    UART_TxCmd(LPC_UART3, ENABLE);
    uart3_flush_rx();
}

static void uart3_write(const void *data, size_t len)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++) {
        while (!(LPC_UART3->LSR & UART_LSR_THRE)) {
        }
        LPC_UART3->THR = p[i];
    }
    while (!(LPC_UART3->LSR & UART_LSR_TEMT)) {
    }
    (void)hal_ms();
}

static void uart3_puts(const char *s)
{
    uart3_write(s, strlen(s));
}

static int uart3_getc(uint32_t timeout_ms)
{
    uint32_t start = hal_ms();
    do {
        if (LPC_UART3->LSR & UART_LSR_RDR) {
            return (int)(LPC_UART3->RBR & 0xFFU);
        }
    } while ((hal_ms() - start) < timeout_ms);
    return -1;
}

/* ---- LPC845 ROM loader (UM11029 ch. 5), as TEST_JIG/migrate/isp845.c ---- */
#define LINE_MAX    64
#define STUB_ADDR   "268437504"     /* 0x10000800 */

static int read_line(char *buf, int size, uint32_t timeout_ms)
{
    int n = 0;
    uint32_t start = hal_ms();
    while ((hal_ms() - start) < timeout_ms) {
        int c = uart3_getc(1);
        if (c < 0) {
            continue;
        }
        if (c == '\n') {
            buf[n] = '\0';
            return n;
        }
        if (c != '\r' && n < size - 1) {
            buf[n++] = (char)c;
        }
    }
    buf[n] = '\0';
    return -1;
}

static bool expect(const char *want, uint32_t timeout_ms)
{
    char line[LINE_MAX];
    uint32_t start = hal_ms();
    for (;;) {
        uint32_t used = hal_ms() - start;
        if (used >= timeout_ms) {
            return false;
        }
        int n = read_line(line, sizeof(line), timeout_ms - used);
        if (n < 0) {
            return false;
        }
        if (n > 0 && strcmp(line, want) == 0) {
            return true;
        }
    }
}

/* PLL0 output / (CCLKCFG + 1). Normal speed is what the startup code set; slow is a quarter of it,
 * for the time the LPC1768 may be on hold-up power. UART3 follows the clock, so it is set again. */
static uint32_t s_cclk_normal;

static void set_cclk_cfg(uint32_t cfg)
{
    (void)hal_ms();                 /* Count the cycles so far at the old speed */
    LPC_SC->CCLKCFG = cfg;
    SystemCoreClockUpdate();
    uart3_open(115200);
}

static void cclk_slow(void)
{
    uint32_t slow = (s_cclk_normal + 1U) * 4U - 1U;
    set_cclk_cfg(slow > 255U ? 255U : slow);
}

static void cclk_normal(void)
{
    set_cclk_cfg(s_cclk_normal);
}

static bool sync(uint32_t total_ms, bool release_pgm_on_sync)
{
    char line[LINE_MAX];
    uint32_t start = hal_ms();
    bool got = false;
    while (!got && (hal_ms() - start) < total_ms) {
        uart3_flush_rx();
        uart3_puts("?");
        int n = read_line(line, sizeof(line), 4);
        got = (n >= 0 && strcmp(line, "Synchronized") == 0);
    }
    if (!got) {
        return false;
    }
    if (release_pgm_on_sync) {
        line_release(LINE_PGM845);  /* Sampled already, before Reinvoke ISP makes it an output */
    }
    uart3_puts("Synchronized\r\n");
    if (!expect("OK", 50)) return false;
    uart3_puts("12000\r\n");
    if (!expect("OK", 50)) return false;
    uart3_puts("A 0\r\n");
    return expect("0", 50);
}

static void drain(uint32_t ms)
{
    uint32_t start = hal_ms();
    while ((hal_ms() - start) < ms) {
        (void)uart3_getc(1);
    }
}

static void fmt_dec(char *out, uint32_t v)
{
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10U);
        v /= 10U;
    } while (v);
    while (n) {
        *out++ = tmp[--n];
    }
    *out = '\0';
}

/* Fast entry at 115200: the rails may float until the stub runs (old power boards cut the LPC1768
 * about 58 ms after the ROM loader starts), then the stub drives them and restarts the loader. */
static bool isp_enter(void)
{
    static uint32_t payload[(G_STUB845_LEN + 3U) / 4U];
    memcpy(payload, g_stub845, G_STUB845_LEN);
    cclk_slow();

    line_drive(LINE_PGM845, false);
    line_drive(LINE_RST845, false);
    hal_delay_ms(2);
    line_release(LINE_RST845);

    bool ok = sync(250, true);
    line_release(LINE_PGM845);
    if (ok) {
        uart3_puts("U 23130\r\n");
        ok = expect("0", 50);
    }
    if (ok) {
        char cmd[40] = "W " STUB_ADDR " ";
        fmt_dec(cmd + strlen(cmd), sizeof(payload));
        strcat(cmd, "\r\n");
        uart3_puts(cmd);
        ok = expect("0", 50);
    }
    if (ok) {
        uart3_write(payload, sizeof(payload));
        drain(3);           /* This ROM answers "0" and a damaged CR here, not "OK" */
        uart3_puts("G " STUB_ADDR " T\r\n");
        drain(30);          /* No clean answer: the stub restarts the loader at once */
    }
    cclk_normal();
    /* Only a restarted loader can answer this, so it proves the stub ran */
    return ok && sync(1000, false);
}

/* The LPC845 restarts its own firmware (the new CTRL checks it for about 25 ms first) */
static void isp_leave(void)
{
    cclk_slow();
    line_release(LINE_PGM845);
    hal_delay_ms(2);
    line_drive(LINE_RST845, false);
    hal_delay_ms(2);
    line_release(LINE_RST845);
    hal_delay_ms(300);
    cclk_normal();
}

static char s_cmd[48];

static void cmd_start(char c)
{
    s_cmd[0] = c;
    s_cmd[1] = '\0';
}

static void cmd_num(uint32_t v)
{
    size_t n = strlen(s_cmd);
    s_cmd[n] = ' ';
    fmt_dec(&s_cmd[n + 1], v);
}

static int cmd_send(uint32_t timeout_ms)
{
    char line[LINE_MAX];
    uart3_flush_rx();
    uart3_puts(s_cmd);
    uart3_puts("\r\n");
    uint32_t start = hal_ms();
    while ((hal_ms() - start) < timeout_ms) {
        int n = read_line(line, sizeof(line), timeout_ms - (hal_ms() - start));
        if (n < 0) {
            return -1;
        }
        if (n == 0) {
            continue;
        }
        if (line[0] < '0' || line[0] > '9') {
            return -1;
        }
        int rc = 0;
        for (int i = 0; line[i] >= '0' && line[i] <= '9'; i++) {
            rc = rc * 10 + (line[i] - '0');
        }
        return rc;
    }
    return -1;
}

static int isp_prepare(uint32_t first, uint32_t last)
{
    cmd_start('P'); cmd_num(first); cmd_num(last);
    return cmd_send(200);
}

static int isp_erase(uint32_t first, uint32_t last)
{
    int rc = isp_prepare(first, last);
    if (rc != 0) {
        return rc;
    }
    cmd_start('E'); cmd_num(first); cmd_num(last);
    return cmd_send(5000);
}

static int isp_blank_check(uint32_t first, uint32_t last)
{
    char line[LINE_MAX];
    cmd_start('I'); cmd_num(first); cmd_num(last);
    int rc = cmd_send(2000);
    if (rc == 8) {          /* SECTOR_NOT_BLANK: offset and contents follow */
        read_line(line, sizeof(line), 100);
        read_line(line, sizeof(line), 100);
    }
    return rc;
}

static bool isp_write_ram(uint32_t ram_addr, const void *data, uint32_t len)
{
    cmd_start('W'); cmd_num(ram_addr); cmd_num(len);
    if (cmd_send(200) != 0) {
        return false;
    }
    uart3_write(data, len);
    char line[LINE_MAX];
    (void)read_line(line, sizeof(line), 5);     /* Measured: usually nothing comes */
    return true;
}

static int isp_copy_to_flash(uint32_t flash_addr, uint32_t ram_addr, uint32_t len)
{
    int rc = isp_prepare(flash_addr / 1024U, (flash_addr + len - 1U) / 1024U);
    if (rc != 0) {
        return rc;
    }
    cmd_start('C'); cmd_num(flash_addr); cmd_num(ram_addr); cmd_num(len);
    return cmd_send(1000);
}

static bool isp_read(uint32_t addr, void *buf, uint32_t len)
{
    cmd_start('R'); cmd_num(addr); cmd_num(len);
    if (cmd_send(200) != 0) {
        return false;
    }
    uint8_t *p = buf;
    for (uint32_t i = 0; i < len; i++) {
        int c = uart3_getc(100);
        if (c < 0) {
            return false;
        }
        p[i] = (uint8_t)c;
    }
    return true;
}

/* ---- sector operations for ctrl_isp_core.c ---- */
#define RAM_BUF     0x10001000UL    /* LPC845 RAM above the ROM loader's 2 KB and the IAP tables */

static bool op_erase(uint32_t sector)
{
    return isp_erase(sector, sector) == 0;
}

static bool op_program(uint32_t sector, const uint8_t *data)
{
    return isp_write_ram(RAM_BUF, data, ISP_SECTOR_BYTES) &&
           isp_copy_to_flash(sector * ISP_SECTOR_BYTES, RAM_BUF, ISP_SECTOR_BYTES) == 0;
}

static bool op_read(uint32_t sector, uint8_t *out)
{
    return isp_read(sector * ISP_SECTOR_BYTES, out, ISP_SECTOR_BYTES);
}

static bool op_blank(uint32_t sector)
{
    return isp_blank_check(sector, sector) == 0;
}

static void op_progress(const char *what, uint32_t sector)
{
    (void)hal_ms();             /* Feeds the watchdog */
    char l2[17];
    snprintf(l2, sizeof(l2), "%.8s %lu/%u", what, (unsigned long)sector, ISP_SECTORS - 1U);
    lcd_show("CTRL CHIP", l2);
}

static void check_progress(const char *stage, unsigned percent)
{
    (void)hal_ms();             /* Feeds the watchdog through the card reads */
    char l2[17];
    snprintf(l2, sizeof(l2), "%.10s %u%%", stage, percent);
    lcd_show("CTRL.HEX", l2);
}

static uint32_t s_fail_sector;

static isp_result_t program_ctrl(uint32_t want_crc)
{
    if (!ctrl_isp_jumpers_fitted()) {
        return ISP_NO_JUMPERS;
    }
    static fw_file_info_t info;
    if (fw_ctrl_check(FW_CTRL_FILENAME, &info, check_progress) != FW_OK) {
        return ISP_FILE_BAD;
    }
    if (info.crc32 != want_crc) {
        return ISP_FILE_CHANGED;
    }

    lcd_show("CTRL CHIP", "ROM LOADER...");
    if (!isp_enter()) {
        return ISP_NO_LOADER;
    }
    strcpy(s_cmd, "U 23130");
    if (cmd_send(200) != 0) {
        return ISP_UNLOCK;
    }
    static const ctrl_isp_ops_t ops = {
        op_erase, op_program, op_read, op_blank, op_progress,
        g_ctrl_boot, G_CTRL_BOOT_LEN, g_rescue845, G_RESCUE845_LEN,
        FW_CTRL_FILENAME, &info
    };
    return ctrl_isp_program(&ops, &s_fail_sector);
}

void ctrl_isp_request(uint32_t ctrl_crc32)
{
    LPC_RTC->GPREG1 = ctrl_crc32;
    LPC_RTC->GPREG0 = ISP_REQ_MAGIC;
}

void ctrl_isp_boot_check(void)
{
    if (LPC_RTC->GPREG0 != ISP_REQ_MAGIC) {
        return;
    }
    uint32_t want_crc = LPC_RTC->GPREG1;
    LPC_RTC->GPREG0 = 0;    /* Once only: a power cut or crash mid-way doesn't loop */

    __disable_irq();        /* Everything here is polled */
    NVIC_DisableIRQ(UART3_IRQn);
    s_cclk_normal = LPC_SC->CCLKCFG;
    timer_start();
    lcd_init();
    lcd_show("CTRL BOOTLOADER", "STARTING");
    s_fail_sector = 0;

    bool session = false;
    isp_result_t r = program_ctrl(want_crc);
    session = (r != ISP_NO_JUMPERS && r != ISP_FILE_BAD && r != ISP_FILE_CHANGED);
    if (session) {
        isp_leave();        /* The LPC845 runs whatever it now has (rescue keeps the rails on) */
    }

    LPC_RTC->GPREG1 = (uint32_t)r;
    LPC_RTC->GPREG2 = s_fail_sector;
    LPC_RTC->GPREG0 = ISP_DONE_MAGIC;

    lcd_show(s_result_lcd[r], "REMOVE JP1+JP2");
    /* Restart once both jumpers are off (the normal firmware must not start with them on) */
    uint32_t off_since = hal_ms();
    for (;;) {
        hal_delay_ms(250);
        if (line_jumper_fitted(LINE_RST845) || line_jumper_fitted(LINE_PGM845)) {
            off_since = hal_ms();
        } else if ((hal_ms() - off_since) >= 1000U) {
            break;
        }
    }
    lcd_show(s_result_lcd[r], "RESTARTING...");
    hal_delay_ms(500);
    NVIC_SystemReset();
}

void ctrl_isp_log_result(void)
{
    if (LPC_RTC->GPREG0 != ISP_DONE_MAGIC) {
        return;
    }
    uint32_t r = LPC_RTC->GPREG1;
    uint32_t sector = LPC_RTC->GPREG2;
    LPC_RTC->GPREG0 = 0;
    char line[112];
    if (r < sizeof(s_result_text) / sizeof(s_result_text[0])) {
        snprintf(line, sizeof(line), "CTRL ROM LOADER: %s", s_result_text[r]);
    } else {
        snprintf(line, sizeof(line), "CTRL ROM LOADER: result %lu", (unsigned long)r);
    }
    if (r >= ISP_SECTOR0 && r <= ISP_VERIFY) {
        size_t n = strlen(line);
        snprintf(line + n, sizeof(line) - n, " (sector %lu)", (unsigned long)sector);
    }
    storage_log_debug(r == ISP_OK ? "FW" : "ERROR", line);
    char history[140];
    snprintf(history, sizeof(history), "Station %s: %s", flash_nvram_get_station_id(), line);
    storage_append_stamped(FW_HISTORY_FILENAME, history);
}
