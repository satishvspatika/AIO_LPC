/* Migration image, survey stage (TEST_JIG.md sections 10-12).
 *
 * Loaded by an old unit's SD bootloader V1.1 from fw.hex into 0x18000. It CANNOT write the
 * LPC1768's flash: the flash functions below refuse. The only writes it makes are to the SD card
 * and, through the ROM loader, none to the LPC845 (read-only queries).
 *
 * What it does:
 *   1. Checks its own image (length and CRC in its image info): fw.hex lines with bad checksums are
 *      skipped by the old bootloader without a word, so a damaged copy must stop here.
 *   2. Says what is at 0x0 (old jump loader or our boot stub) and at 0xB000 (old bootloader).
 *   3. Copies the old flash 0x0-0x17FFF (settings, day data, old bootloader) to MIGRATE/OLDFLASH.BIN.
 *   4. Senses JP1 and JP2.
 *   5. With both fitted: puts the LPC845 in its ROM loader, reads part ID, boot version, serial
 *      number, then restarts the LPC845's own firmware.
 *   6. Writes MIGRATE/SURVEY.TXT and shows the result on the LCD.
 * It never sends a CTRL command, so the old CTRL keeps the rails on (up to 32 minutes). */
#include <string.h>
#include "LPC17xx.h"
#include "hal.h"
#include "isp845.h"
#include "ff.h"
#include "fw_layout.h"
#include "boot_core.h"

#define MIG_VERSION "MIG-S0.8"

const fw_image_info_t g_fw_image_info = {
    FW_IMAGE_MAGIC, FW_BOARD_ID, 0, 0, 0xFFFFFFFFUL, MIG_VERSION   /* Length and CRC: build.sh */
};

/* ---- Flash access for boot_core.c / fw_journal.c: read only in the survey ---- */
const uint8_t *fw_hal_ptr(uint32_t addr)
{
    return (const uint8_t *)addr;
}

bool fw_hal_erase(uint32_t sector)
{
    (void)sector;
    return false;
}

bool fw_hal_program(uint32_t addr, const void *src, uint32_t len)
{
    (void)addr; (void)src; (void)len;
    return false;
}

/* ---- report text ---- */
static char s_report[3072];

static void rep(const char *s)
{
    strncat(s_report, s, sizeof(s_report) - 1 - strlen(s_report));
}

static void rep_kv_hex(const char *key, uint32_t v)
{
    char buf[12];
    rep(key);
    rep("0x");
    fmt_hex(buf, v, 8);
    rep(buf);
    rep("\r\n");
}

static uint32_t rd32(uint32_t addr)
{
    return *(const volatile uint32_t *)addr;
}

/* Our boot stub: SP 0x10008000, then a reset handler inside its first 64 bytes */
static const char *sector0_kind(void)
{
    uint32_t sp = rd32(0x0), reset = rd32(0x4);
    if (sp == 0x10008000UL && reset > 0x8 && reset < 0x40 && rd32(0x0C) == 0 && rd32(0x10) == 0) {
        return "NEW BOOT STUB";
    }
    if (sp >= 0x10000000UL && sp <= 0x10008000UL && reset < 0x1000) {
        return "OLD JUMP LOADER";
    }
    return "UNKNOWN";
}

static bool old_bootloader_present(void)
{
    uint32_t sp = rd32(0xB000), reset = rd32(0xB004);
    return sp >= 0x10000000UL && sp <= 0x10008000UL && reset >= 0xB000 && reset < 0x18000;
}

/* Printable runs of 4+ characters in the old settings sector, for the report */
static void rep_settings_strings(void)
{
    char run[40];
    int n = 0;
    for (uint32_t a = 0x1000; a < 0x2000; a++) {
        char c = *(const char *)a;
        if (c >= 0x20 && c < 0x7F && n < (int)sizeof(run) - 1) {
            run[n++] = c;
            continue;
        }
        if (n >= 4) {
            char addr[8];
            run[n] = '\0';
            rep("  ");
            fmt_hex(addr, a - (uint32_t)n, 4);
            rep(addr);
            rep(" ");
            rep(run);
            rep("\r\n");
        }
        n = 0;
    }
}

static FATFS s_fs;
static FIL s_file;

static bool write_file(const char *path, const void *data, uint32_t len)
{
    UINT done = 0;
    if (f_open(&s_file, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
        return false;
    }
    FRESULT r = f_write(&s_file, data, len, &done);
    FRESULT c = f_close(&s_file);
    return r == FR_OK && c == FR_OK && done == len;
}

static void stop(const char *l1, const char *l2)
{
    lcd_show(l1, l2);
    for (;;) {
    }
}

int main(void)
{
    hal_init();
    lcd_init();
    lcd_show("CONVERT SURVEY", MIG_VERSION);
    hal_delay_ms(1500);

    /* 1. Own image */
    rep("Migration survey " MIG_VERSION "\r\n");
    bool self_ok = fw_image_valid(FW_SLOT_A_BASE);
    rep(self_ok ? "Image check: OK\r\n" : "Image check: FAILED\r\n");
    if (!self_ok) {
        stop("IMAGE DAMAGED", "COPY fw.hex AGAIN");
    }

    /* 2. What is on this chip */
    rep("Sector 0: ");
    rep(sector0_kind());
    rep("\r\n");
    rep_kv_hex("  SP    ", rd32(0x0));
    rep_kv_hex("  reset ", rd32(0x4));
    rep(old_bootloader_present() ? "Old bootloader at 0xB000: present\r\n" : "Old bootloader at 0xB000: NOT FOUND\r\n");
    rep_kv_hex("  reset ", rd32(0xB004));
    rep_kv_hex("Code protection word 0x2FC: ", rd32(0x2FC));
    rep("Old settings (0x1000) text:\r\n");
    rep_settings_strings();

    /* 4. Jumpers */
    bool jp1 = line_jumper_fitted(LINE_RST845);
    bool jp2 = line_jumper_fitted(LINE_PGM845);
    rep(jp1 ? "JP1 (RST_845): fitted\r\n" : "JP1 (RST_845): open\r\n");
    rep(jp2 ? "JP2 (PGM_845): fitted\r\n" : "JP2 (PGM_845): open\r\n");

    /* 5. LPC845 ROM loader */
    bool isp_ok = false;
    static isp_survey_t isp;
    if (jp1 && jp2) {
        lcd_show("CTRL CHIP", "ROM LOADER...");
        isp_ok = isp_enter(&isp);
        if (isp_ok) {
            isp_read_ids(&isp);
        }
        isp_leave();
        char num[12];
        rep("LPC845 ROM loader: ");
        rep(isp_ok ? "OK, rails held by the RAM stub\r\n" : "FAILED\r\n");
        rep("  last step: "); fmt_dec(num, (uint32_t)isp.step); rep(num);
        rep(" (1 sync, 2 echo off, 3 unlock, 4 stub written, 5 stub started, 6 resynced)\r\n");
        rep("  ms to first Synchronized: "); fmt_dec(num, isp.ms_to_sync); rep(num); rep("\r\n");
        rep("  ms rails floating (reset to stub started): "); fmt_dec(num, isp.ms_rails_floating); rep(num); rep("\r\n");
        rep("  replies: "); rep(isp.note); rep("\r\n");
        if (isp_ok) {
            rep("  J (part ID):   "); rep(isp.part_id); rep("\r\n");
            rep("  K (boot ver):  "); rep(isp.boot_ver); rep("\r\n");
            rep("  N (serial):    "); rep(isp.uid); rep("\r\n");
        }
        rep("LPC845 restarted with its own firmware\r\n");
    } else {
        rep("LPC845 ROM loader: not tried (JP1 + JP2 needed)\r\n");
    }

    /* 3. Copy of the old flash */
    lcd_show("SD CARD", "SAVING OLD FLASH");
    bool sd_ok = (f_mount(0, &s_fs) == FR_OK);
    if (sd_ok) {
        FRESULT m = f_mkdir("MIGRATE");
        sd_ok = (m == FR_OK || m == FR_EXIST);
    }
    bool saved = sd_ok && write_file("MIGRATE/OLDFLASH.BIN", (const void *)0x0, 0x18000);
    rep(saved ? "Old flash 0x0-0x17FFF saved: MIGRATE/OLDFLASH.BIN\r\n" : "Old flash copy: FAILED (SD card)\r\n");

    /* 6. Report */
    bool rep_ok = sd_ok && write_file("MIGRATE/SURVEY.TXT", s_report, strlen(s_report));

    if (!sd_ok || !rep_ok || !saved) {
        stop("SURVEY: SD FAIL", "CHECK FAT32 CARD");
    }
    if (!(jp1 && jp2)) {
        stop("SURVEY SAVED", jp1 ? "JP2 NOT FITTED" : "JP1 NOT FITTED");
    }
    if (!isp_ok) {
        stop("SURVEY SAVED", "CTRL NO ANSWER");
    }
    stop("SURVEY OK", "REMOVE JP1+JP2");
    return 0;
}
