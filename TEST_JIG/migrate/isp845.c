/* LPC845 ROM loader over UART3 (NXP UM11029 rev 1.7, chapter 5):
 *   5.4.1 '?' -> "Synchronized"; host echoes it -> "OK"; clock in kHz -> "OK" (ignored by this chip)
 *   5.5.3 "A 0" echo off; 5.5.1 "U 23130" unlocks write and Go; 5.5.4 "W <addr> <n>" then n bytes
 *         of plain binary, n a multiple of 4; 5.5.8 "G <addr> T"
 *   5.3.7.2 the loader uses RAM 0x10000000-0x100007FF; the stub goes to 0x10000800
 *   5.6.8 IAP 57 (Reinvoke ISP), param 1 = USART: the stub calls it after driving the rail lines
 * Commands answer a return code line ("0" = CMD_SUCCESS). With echo on, the command comes back first.
 *
 * Old units' power boards switch the LPC1768 off while the LPC845's rail lines float; the LPC1768
 * survives at least 300 ms of that (measured), so everything up to the stub's start is done at
 * 115200 in one go, about 30 ms of traffic. */
#include "isp845.h"
#include "hal.h"
#include "stub845_bin.h"   /* build.sh: g_stub845[], G_STUB845_LEN, from stub845.s */
#include <string.h>
#include "LPC17xx.h"
#include "system_LPC17xx.h"

#define LINE_MAX     64
#define STUB_ADDR    "268437504"   /* 0x10000800 */

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

static void append(char *dst, const char *s)
{
    size_t used = strlen(dst);
    if (used && used < ISP_RAW_MAX - 1) {
        dst[used++] = '|';
        dst[used] = '\0';
    }
    strncat(dst, s, ISP_RAW_MAX - 1 - used);
}

/* Reads lines until one equals want, or the time runs out. Non-empty lines go into raw. */
static bool expect(const char *want, uint32_t timeout_ms, char *raw)
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
            if (line[0] && raw) append(raw, line);
            return false;
        }
        if (n == 0) {
            continue;
        }
        if (raw) append(raw, line);
        if (strcmp(line, want) == 0) {
            return true;
        }
    }
}

/* Autobaud: '?' until "Synchronized" (the ROM may still be starting), then the rest */
static uint32_t s_q_sent;
static void (*s_on_sync)(void);

static bool sync(uint32_t total_ms, char *raw, uint32_t *ms_taken)
{
    char line[LINE_MAX];
    uint32_t start = hal_ms();
    bool got = false;
    s_q_sent = 0;
    while (!got && (hal_ms() - start) < total_ms) {
        uart3_flush_rx();
        uart3_puts("?");
        s_q_sent++;
        int n = read_line(line, sizeof(line), 4);
        got = (n >= 0 && strcmp(line, "Synchronized") == 0);
    }
    if (ms_taken) *ms_taken = hal_ms() - start;
    if (!got) {
        append(raw, "no Synchronized");
        return false;
    }
    if (s_on_sync) s_on_sync();
    uart3_puts("Synchronized\r\n");
    if (!expect("OK", 50, raw)) return false;
    uart3_puts("12000\r\n");
    if (!expect("OK", 50, raw)) return false;
    uart3_puts("A 0\r\n");
    return expect("0", 50, raw);
}

/* Everything received for ms milliseconds, as hex, into the report */
static void raw_bytes(char *raw, const char *label, uint32_t ms)
{
    char hex[3 * 24 + 1];
    int n = 0;
    uint32_t start = hal_ms();
    while ((hal_ms() - start) < ms) {
        int c = uart3_getc(1);
        if (c >= 0 && n < 24) {
            fmt_hex(&hex[3 * n], (uint32_t)c, 2);
            hex[3 * n + 2] = ' ';
            n++;
        }
    }
    hex[3 * n] = '\0';
    append(raw, label);
    append(raw, n ? hex : "(nothing)");
}

/* Timings (ms from the start of the reset pulse) travel to the LPC845 with the stub, after it, so a
 * run that loses power can still be read back with the probe: magic 'TIME', then the values. */
#define TIME_WORDS 0
static uint32_t s_payload[(G_STUB845_LEN / 4U) + TIME_WORDS];

/* CPU clock for the hold-up window: PLL0 runs at 400 MHz, CCLKCFG divides it (value + 1).
 * 3 = 100 MHz (normal), 15 = 25 MHz (about a quarter of the current, so the rail lasts longer).
 * UART3 and SysTick follow the clock, so both are set again after each change. */
static void set_cclk_div(uint32_t cfg, uint32_t baud)
{
    LPC_SC->CCLKCFG = cfg;
    SystemCoreClockUpdate();
    SysTick_Config(SystemCoreClock / 1000U);
    uart3_open(baud);
}

static void release_pgm(void)
{
    line_release(LINE_PGM845);             /* Sampled already; before Reinvoke ISP makes it an output */
}

bool isp_enter(isp_survey_t *out)
{
    memset(out, 0, sizeof(*out));
    out->baud = 115200;
    set_cclk_div(15, 115200);              /* 25 MHz until the stub holds the rails */
    memcpy(s_payload, g_stub845, G_STUB845_LEN);
    static uint32_t tm[8];                 /* Kept here now: the payload is the stub alone, for speed */
    memset(tm, 0, sizeof(tm));

    /* From here the rail lines float: the LPC1768 runs on its hold-up until the stub starts */
    line_drive(LINE_PGM845, false);
    line_drive(LINE_RST845, false);
    uint32_t t0 = hal_ms();
    hal_delay_ms(2);
    line_release(LINE_RST845);
    tm[1] = hal_ms() - t0;                 /* reset released */

    s_on_sync = release_pgm;
    bool ok = sync(250, out->note, &out->ms_to_sync);
    s_on_sync = NULL;
    release_pgm();
    tm[2] = hal_ms() - t0;                 /* synchronised (or gave up) */
    tm[3] = s_q_sent;
    if (!ok) {
        return false;
    }
    out->step = 2;
    tm[4] = hal_ms() - t0;

    uart3_puts("U 23130\r\n");
    if (!expect("0", 50, out->note)) return false;
    out->step = 3;
    tm[5] = hal_ms() - t0;                 /* unlocked */

#ifdef MIG_HOLDUP_PROBE
    /* Hold-up measurement: one long W into 0x10001000, a word of "ms since the reset pulse" sent
     * each millisecond for 2 s. The ROM stores each word as it arrives, so after a power loss the
     * last word there (read with the probe) is when the LPC1768 stopped. */
    uart3_puts("W 268439552 8000\r\n");
    if (!expect("0", 50, out->note)) return false;
    for (uint32_t i = 0; i < 2000; i++) {
        uint32_t now = hal_ms();
        while (hal_ms() == now) {
        }
        uint32_t stamp = hal_ms() - t0;
        uart3_write(&stamp, 4);
    }
    if (!expect("OK", 200, out->note)) append(out->note, "(no OK after W data)");
    append(out->note, "holdup: survived 2 s");
#endif

    char cmd[40] = "W " STUB_ADDR " ";
    fmt_dec(cmd + strlen(cmd), sizeof(s_payload));
    strcat(cmd, "\r\n");
    uart3_puts(cmd);
    if (!expect("0", 50, out->note)) return false;
    tm[6] = hal_ms() - t0;                 /* W accepted, data about to go */
    uart3_write(s_payload, sizeof(s_payload));
    /* Measured: after the data this ROM answers "0" and a damaged CR (0x8D), not "OK"; and "G"
     * gets no clean answer, likely because the stub restarts the loader (and its USART) at once.
     * So: keep the raw bytes for the report, send "G", then prove the stub ran by synchronising
     * again: only a restarted loader can answer. */
    raw_bytes(out->note, "after W:", 3);
    out->step = 4;

    uart3_puts("G " STUB_ADDR " T\r\n");
    raw_bytes(out->note, "after G:", 30);
    out->ms_rails_floating = hal_ms() - t0;
    set_cclk_div(3, 115200);               /* Back to 100 MHz */
    out->step = 5;

    append(out->note, "resync:");
    if (!sync(1000, out->note, NULL)) return false;
    out->step = 6;
    return true;
}

/* ---- commands after the session is up (UM11029 5.5) ---- */
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

/* Sends s_cmd and reads the return code line (blank lines skipped) */
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

int isp_unlock(void)
{
    strcpy(s_cmd, "U 23130");
    return cmd_send(200);
}

int isp_prepare(uint32_t first_sector, uint32_t last_sector)
{
    cmd_start('P'); cmd_num(first_sector); cmd_num(last_sector);
    return cmd_send(200);
}

int isp_erase(uint32_t first_sector, uint32_t last_sector)
{
    int rc = isp_prepare(first_sector, last_sector);
    if (rc != 0) {
        return rc;
    }
    cmd_start('E'); cmd_num(first_sector); cmd_num(last_sector);
    return cmd_send(5000);
}

int isp_blank_check(uint32_t first_sector, uint32_t last_sector)
{
    char line[LINE_MAX];
    cmd_start('I'); cmd_num(first_sector); cmd_num(last_sector);
    int rc = cmd_send(2000);
    if (rc == 8) {                      /* SECTOR_NOT_BLANK: offset and contents follow */
        read_line(line, sizeof(line), 100);
        read_line(line, sizeof(line), 100);
    }
    return rc;
}

bool isp_write_ram(uint32_t ram_addr, const void *data, uint32_t len)
{
    cmd_start('W'); cmd_num(ram_addr); cmd_num(len);
    if (cmd_send(200) != 0) {
        return false;
    }
    uart3_write(data, len);
    /* Measured: this ROM sends nothing after the data (UM11029 says "OK"); take it if it comes */
    char line[LINE_MAX];
    (void)read_line(line, sizeof(line), 5);
    return true;
}

int isp_copy_to_flash(uint32_t flash_addr, uint32_t ram_addr, uint32_t len)
{
    uint32_t first = flash_addr / 1024U, last = (flash_addr + len - 1U) / 1024U;
    int rc = isp_prepare(first, last);
    if (rc != 0) {
        return rc;
    }
    cmd_start('C'); cmd_num(flash_addr); cmd_num(ram_addr); cmd_num(len);
    return cmd_send(1000);
}

bool isp_read(uint32_t addr, void *buf, uint32_t len)
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

static void query(const char *cmd, int data_lines, char *raw)
{
    char line[LINE_MAX];
    uart3_flush_rx();
    uart3_puts(cmd);
    uart3_puts("\r\n");
    for (int i = 0; i < data_lines + 1; i++) {   /* Return code, then the data */
        if (read_line(line, sizeof(line), 500) < 0) {
            append(raw, "(timeout)");
            return;
        }
        append(raw, line);
    }
}

void isp_read_ids(isp_survey_t *out)
{
    query("J", 1, out->part_id);
    query("K", 2, out->boot_ver);
    query("N", 4, out->uid);
}

void isp_leave(void)
{
    /* The LPC845 restarts its own firmware: the old CTRL drives the rails within a few ms, the new
     * CTRL's bootloader first checks the firmware's CRC (about 25 ms). Run slowly across it. */
    set_cclk_div(15, 115200);
    line_release(LINE_PGM845);
    hal_delay_ms(2);
    line_drive(LINE_RST845, false);
    hal_delay_ms(2);
    line_release(LINE_RST845);
    hal_delay_ms(300);
    set_cclk_div(3, 115200);
}
