/* LPC845 flash image and write order for the CTRL ROM loader route (ctrl_isp_core.h). Logic as
 * TEST_JIG/migrate/ctrl845.c (MIG-1.4, proven on old unit 2114).
 *
 * Result, the same as LPC845_TWS.hex from the MCU-Link: CTRL bootloader at 0, firmware at 0x2400
 * (FW845_SLOT_A_BASE), every other byte 0xFF (so the CTRL update journal starts empty).
 * Order, so a power cut anywhere leaves an LPC845 the operator can run this on again:
 *   1. Sector 0 <- rescue program: at power-up it keeps the rails on and opens the ROM loader.
 *   2. Sectors 1-63: bootloader tail, blanks, firmware, blanks; each read back.
 *   3. Sector 0 <- the CTRL bootloader's head, read back. Then the whole chip is compared once more.
 * Sector 0 is the only one the LPC845 boots from, and it holds either the rescue program or the
 * final bootloader at every moment except inside its own erase-to-write window, or (if writing it
 * keeps failing) is left erased, so the boot ROM opens its loader. Either way JP1+JP2 and CTRL F/W
 * again start over: the LPC1768 forces the ROM loader with -PGM_845 low whatever sector 0 holds. */
#include "ctrl_isp_core.h"
#include "boot/fw_layout.h"
#include <string.h>

#define SECTOR      ISP_SECTOR_BYTES
#define SECTORS     ISP_SECTORS
#define APP_SECTOR  (FW845_SLOT_A_BASE / SECTOR)    /* 9 */

static const ctrl_isp_ops_t *s_ops;
static uint8_t s_buf[SECTOR];
static uint8_t s_want[SECTOR];
static uint32_t s_fail_sector;

static bool all_ff(const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (p[i] != 0xFF) return false;
    }
    return true;
}

/* Sectors below the firmware: the CTRL bootloader, then blank */
static void boot_sector(uint32_t sector, uint8_t *out)
{
    memset(out, 0xFF, SECTOR);
    uint32_t base = sector * SECTOR;
    if (base < s_ops->boot_len) {
        uint32_t n = s_ops->boot_len - base;
        memcpy(out, &s_ops->boot[base], n > SECTOR ? SECTOR : n);
    }
}

static void rescue_sector(uint8_t *out)
{
    memset(out, 0xFF, SECTOR);
    memcpy(out, s_ops->rescue, s_ops->rescue_len > SECTOR ? SECTOR : s_ops->rescue_len);
}

static void progress(const char *what, uint32_t sector)
{
    if (s_ops->progress) {
        s_ops->progress(what, sector);
    }
}

static bool same_as_chip(uint32_t sector, const uint8_t *data)
{
    if (all_ff(data, SECTOR)) {
        return s_ops->blank(sector);
    }
    return s_ops->read(sector, s_buf) && memcmp(s_buf, data, SECTOR) == 0;
}

/* One sector: erase, write, read back; 3 tries, 6 for sector 0. If sector 0 still fails it is left
 * erased: with no valid vector table the LPC845 boot ROM opens its own loader (UM11029 5.3.4), the
 * state a rerun starts from. A half-written sector 0 with a valid table could run broken code. */
static bool write_sector(uint32_t sector, const uint8_t *data)
{
    int tries = (sector == 0) ? 6 : 3;
    for (int attempt = 0; attempt < tries; attempt++) {
        if (!s_ops->erase(sector)) {
            continue;
        }
        if (!all_ff(data, SECTOR) && !s_ops->program(sector, data)) {
            continue;
        }
        if (same_as_chip(sector, data)) {
            return true;
        }
    }
    if (sector == 0) {
        (void)s_ops->erase(0);
    }
    s_fail_sector = sector;
    return false;
}

/* The firmware arrives from ctrl.hex in order (fw_ctrl_stream refuses gaps and going backwards),
 * 256 bytes at a time, and goes out a sector at a time. The same path compares at the end. */
static bool s_verify;
static uint32_t s_app_sector;   /* Sector being filled */
static uint32_t s_app_end;      /* First sector after the firmware */
static bool s_app_ok;

static bool app_flush(void)
{
    progress(s_verify ? "CHECK" : "WRITE", s_app_sector);
    bool ok = s_verify ? same_as_chip(s_app_sector, s_want) : write_sector(s_app_sector, s_want);
    if (!ok) {
        s_fail_sector = s_app_sector;
    }
    s_app_sector++;
    memset(s_want, 0xFF, SECTOR);
    return ok;
}

static bool app_sink(uint32_t offset, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        uint32_t addr = FW845_SLOT_A_BASE + offset + i;
        uint32_t sector = addr / SECTOR;
        if (sector < s_app_sector || addr >= FW845_SLOT_A_BASE + FW845_SLOT_SIZE) {
            s_app_ok = false;   /* Never write a byte into a sector already done, or past slot A */
            return false;
        }
        while (sector > s_app_sector) {
            if (!app_flush()) {
                s_app_ok = false;
                return false;
            }
        }
        s_want[addr % SECTOR] = data[i];
    }
    return true;
}

static bool app_pass(bool verify)
{
    s_verify = verify;
    s_app_sector = APP_SECTOR;
    s_app_ok = true;
    memset(s_want, 0xFF, SECTOR);
    fw_result_t r = fw_ctrl_stream(s_ops->ctrl_file, s_ops->ctrl_info, app_sink, 256, NULL);
    if (r != FW_OK || !s_app_ok) {
        return false;
    }
    if (!app_flush()) {         /* The last, part-filled sector */
        return false;
    }
    s_app_end = s_app_sector;
    return true;
}

isp_result_t ctrl_isp_program(const ctrl_isp_ops_t *ops, uint32_t *fail_sector)
{
    s_ops = ops;
    s_fail_sector = 0;
    isp_result_t r = ISP_OK;

    if (ops->boot_len == 0 || ops->boot_len > FW845_SLOT_A_BASE || ops->rescue_len == 0 || ops->rescue_len > SECTOR) {
        r = ISP_WRITE;          /* Built wrong: write nothing */
        goto done;
    }

    /* 1. Rescue sector 0, unless an earlier run left it there */
    rescue_sector(s_want);
    if (!(ops->read(0, s_buf) && memcmp(s_buf, s_want, SECTOR) == 0)) {
        progress("RESCUE", 0);
        if (!write_sector(0, s_want)) {
            r = ISP_SECTOR0;
            goto done;
        }
    }

    /* 2. Sectors 1-63 */
    for (uint32_t s = 1; s < APP_SECTOR; s++) {
        progress("WRITE", s);
        boot_sector(s, s_want);
        if (!write_sector(s, s_want)) {
            r = ISP_WRITE;
            goto done;
        }
    }
    if (!app_pass(false)) {
        r = ISP_WRITE;
        goto done;
    }
    memset(s_want, 0xFF, SECTOR);
    for (uint32_t s = s_app_end; s < SECTORS; s++) {
        progress("WRITE", s);
        if (!write_sector(s, s_want)) {
            r = ISP_WRITE;
            goto done;
        }
    }

    /* 3. Sector 0 last */
    progress("SECTOR 0", 0);
    boot_sector(0, s_want);
    if (!write_sector(0, s_want)) {
        rescue_sector(s_want);
        (void)write_sector(0, s_want);
        s_fail_sector = 0;
        r = ISP_SECTOR0;
        goto done;
    }

    /* Whole chip once more */
    for (uint32_t s = 0; s < APP_SECTOR; s++) {
        progress("CHECK", s);
        boot_sector(s, s_want);
        if (!same_as_chip(s, s_want)) {
            s_fail_sector = s;
            r = ISP_VERIFY;
            goto done;
        }
    }
    if (!app_pass(true)) {
        r = ISP_VERIFY;
        goto done;
    }
    memset(s_want, 0xFF, SECTOR);
    for (uint32_t s = s_app_end; s < SECTORS; s++) {
        progress("CHECK", s);
        if (!same_as_chip(s, s_want)) {
            s_fail_sector = s;
            r = ISP_VERIFY;
            goto done;
        }
    }

done:
    if (fail_sector) {
        *fail_sector = s_fail_sector;
    }
    return r;
}
