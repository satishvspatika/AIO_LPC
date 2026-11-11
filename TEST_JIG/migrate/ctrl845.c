/* M1: LPC845 to the new CTRL (bootloader at 0x0, firmware at 0x2400, everything else blank: the
 * same as LPC845/LPC845_TWS.hex from the MCU-Link), through the ROM loader. Safe to run again.
 *
 * Order, so a power cut at any point leaves a unit that comes back by itself:
 *   1. Read all 64 sectors; save them if they still hold the old firmware; stop here if they
 *      already match the new CTRL.
 *   2. Sector 0 <- the rescue program (rescue845.s): from now on a power-up drives the rails and
 *      opens the ROM loader, so the LPC1768 restarts the migration.
 *   3. Sectors 1-63: erase, write the CTRL bootloader's tail and the firmware, read back.
 *   4. Sector 0 <- the CTRL bootloader's head, read back.
 * The only unsafe moments are 2 and 4, between erasing sector 0 and writing it (about 20 ms each). */
#include "ctrl845.h"
#include "isp845.h"
#include "hal.h"
#include "ctrl_img.h"   /* build.sh: g_ctrl_boot[], g_ctrl_app[], g_rescue845[], their lengths, G_CTRL_VERSION */
#include <string.h>

#define SECTOR      1024U
#define SECTORS     64U
#define APP_BASE    0x2400U
#define RAM_BUF     0x10001000UL   /* Above the ROM loader's 2 KB and the IAP tables at 0xF00 */

static uint8_t s_buf[SECTOR];
static uint8_t s_want[SECTOR];

const char *m1_ctrl_version(void)
{
    return G_CTRL_VERSION;
}

/* What the new CTRL's flash holds at this sector */
static void target_sector(uint32_t sector, uint8_t *out)
{
    uint32_t base = sector * SECTOR;
    memset(out, 0xFF, SECTOR);
    for (uint32_t i = 0; i < SECTOR; i++) {
        uint32_t a = base + i;
        if (a < G_CTRL_BOOT_LEN) {
            out[i] = g_ctrl_boot[a];
        } else if (a >= APP_BASE && a < APP_BASE + G_CTRL_APP_LEN) {
            out[i] = g_ctrl_app[a - APP_BASE];
        }
    }
}

static bool all_ff(const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (p[i] != 0xFF) return false;
    }
    return true;
}

static void rescue_sector(uint8_t *out)
{
    memset(out, 0xFF, SECTOR);
    memcpy(out, g_rescue845, G_RESCUE845_LEN);
}

static void progress(const char *what, uint32_t n, uint32_t of)
{
    char l2[17], num[12];
    strcpy(l2, what);
    fmt_dec(num, n);
    strcat(l2, num);
    strcat(l2, "/");
    fmt_dec(num, of);
    strcat(l2, num);
    lcd_show("CTRL CHIP", l2);
}

/* One sector: erase, write, read back; tries 3 times */
static bool write_sector(uint32_t sector, const uint8_t *data, m1_note_fn note)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        if (isp_erase(sector, sector) != 0) {
            continue;
        }
        if (!all_ff(data, SECTOR)) {
            if (!isp_write_ram(RAM_BUF, data, SECTOR)) continue;
            if (isp_copy_to_flash(sector * SECTOR, RAM_BUF, SECTOR) != 0) continue;
        }
        if (isp_read(sector * SECTOR, s_buf, SECTOR) && memcmp(s_buf, data, SECTOR) == 0) {
            return true;
        }
    }
    char t[32] = "sector ";
    fmt_dec(t + strlen(t), sector);
    strcat(t, " failed 3 times");
    note(t);
    return false;
}

m1_result_t m1_program_ctrl(m1_save_fn save_old, m1_note_fn note)
{
    if (isp_unlock() != 0) {
        note("unlock refused");
        return M1_FAIL_READ;
    }

    /* 1. What is there now */
    uint64_t differs = 0;
    bool sector0_rescue = false, sector0_new = false, saving = (save_old != NULL);
    rescue_sector(s_want);
    for (uint32_t s = 0; s < SECTORS; s++) {
        progress("READ ", s + 1, SECTORS);
        if (!isp_read(s * SECTOR, s_buf, SECTOR)) {
            note("read failed");
            return M1_FAIL_READ;
        }
        if (s == 0) {
            sector0_rescue = (memcmp(s_buf, s_want, SECTOR) == 0);
            target_sector(0, s_want);
            sector0_new = (memcmp(s_buf, s_want, SECTOR) == 0);
            if (sector0_rescue || sector0_new) {
                saving = false;   /* Not the old firmware any more: an earlier run saved it */
            }
        }
        if (saving && !save_old(s * SECTOR, s_buf, SECTOR)) {
            saving = false;
            note("old CTRL copy to SD failed");
        }
        target_sector(s, s_want);
        if (memcmp(s_buf, s_want, SECTOR) != 0) {
            differs |= (1ULL << s);
        }
    }
    note(sector0_rescue ? "found: rescue sector 0 (earlier run cut short)" :
         sector0_new ? "found: new CTRL sector 0" : "found: old CTRL");
    if (differs == 0) {
        return M1_ALREADY;
    }

    /* 2. Rescue sector 0 */
    if (!sector0_rescue) {
        lcd_show("CTRL CHIP", "RESCUE SECTOR");
        rescue_sector(s_want);
        if (!write_sector(0, s_want, note)) {
            return M1_FAIL_SECTOR0;
        }
        note("rescue sector 0 written");
    }

    /* 3. Sectors 1-63 */
    for (uint32_t s = 1; s < SECTORS; s++) {
        progress("WRITE ", s, SECTORS - 1);
        target_sector(s, s_want);
        if (!write_sector(s, s_want, note)) {
            return M1_FAIL_WRITE;
        }
    }
    note("sectors 1-63 written and read back");

    /* 4. Sector 0 last */
    lcd_show("CTRL CHIP", "SECTOR 0");
    target_sector(0, s_want);
    if (!write_sector(0, s_want, note)) {
        /* Leave the rescue sector, so the unit still comes back at power-up */
        rescue_sector(s_want);
        (void)write_sector(0, s_want, note);
        return M1_FAIL_SECTOR0;
    }

    /* Whole chip once more */
    for (uint32_t s = 0; s < SECTORS; s++) {
        progress("CHECK ", s + 1, SECTORS);
        target_sector(s, s_want);
        if (!isp_read(s * SECTOR, s_buf, SECTOR) || memcmp(s_buf, s_want, SECTOR) != 0) {
            char t[32] = "check failed at sector ";
            fmt_dec(t + strlen(t), s);
            note(t);
            return M1_FAIL_VERIFY;
        }
    }
    note("whole chip read back: matches the new CTRL");
    return M1_DONE;
}
