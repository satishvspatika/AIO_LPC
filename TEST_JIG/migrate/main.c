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
 *   3. Copies the old flash 0x0-0x17FFF (settings, day data, old bootloader) to MIGRATE/<ID>/OLDFLASH.BIN.
 *   4. Senses JP1 and JP2.
 *   5. With both fitted: puts the LPC845 in its ROM loader, reads part ID, boot version, serial
 *      number, then restarts the LPC845's own firmware.
 *   6. Writes MIGRATE/SURVEY.TXT and shows the result on the LCD.
 * It never sends a CTRL command, so the old CTRL keeps the rails on (up to 32 minutes). */
#include <string.h>
#include "LPC17xx.h"
#include "hal.h"
#include "isp845.h"
#include "ctrl845.h"
#include "m2.h"
#include "ff.h"
#include "fw_layout.h"
#include "boot_core.h"

#define MIG_VERSION "MIG-1.2"

const fw_image_info_t g_fw_image_info = {
    FW_IMAGE_MAGIC, FW_BOARD_ID, 0, 0, 0xFFFFFFFFUL, MIG_VERSION   /* Length and CRC: build.sh */
};

/* ---- report text ---- */
static char s_report[3072];

static void rep(const char *s)
{
    strncat(s_report, s, sizeof(s_report) - 1 - strlen(s_report));
}

__attribute__((unused)) static void rep_kv_hex(const char *key, uint32_t v)
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
__attribute__((unused)) static void rep_settings_strings(void)
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

static bool file_exists(const char *path)
{
    if (f_open(&s_file, path, FA_OPEN_EXISTING | FA_READ) != FR_OK) {
        return false;
    }
    f_close(&s_file);
    return true;
}

/* Each unit's files go in MIGRATE/<station ID>/, so one card serves many units: a rerun on the
 * same unit (after a power cut) finds and keeps its own backups, another unit gets a new folder.
 * The ID is the old settings' first field (0x1000, e.g. "002114"); sector 1 is never changed by
 * the conversion, so a rerun reads the same ID. */
static char s_dir[24] = "MIGRATE/NOID";
static char s_path[40];

static void set_unit_dir(void)
{
    char id[9];
    int n = 0;
    for (const char *p = (const char *)0x1000; n < 8; p++) {
        char c = *p;
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) break;
        id[n++] = c;
    }
    id[n] = '\0';
    if (n > 0) {
        strcpy(s_dir, "MIGRATE/");
        strcat(s_dir, id);
    }
}

static const char *unit_path(const char *name)
{
    strcpy(s_path, s_dir);
    strcat(s_path, "/");
    strcat(s_path, name);
    return s_path;
}

/* M1 hands over the old CTRL's flash a sector at a time: MIGRATE/<ID>/OLD845.BIN */
static FIL s_old845;
static bool s_old845_open;

static bool save_old845(uint32_t addr, const uint8_t *data, uint32_t len)
{
    UINT done = 0;
    if (addr == 0) {
        s_old845_open = (f_open(&s_old845, unit_path("OLD845.BIN"), FA_CREATE_ALWAYS | FA_WRITE) == FR_OK);
    }
    if (!s_old845_open || f_write(&s_old845, data, len, &done) != FR_OK || done != len) {
        return false;
    }
    if (addr + len >= 0x10000U) {
        s_old845_open = false;
        return f_close(&s_old845) == FR_OK;
    }
    return true;
}

static void note_line(const char *text)
{
    rep("  ");
    rep(text);
    rep("\r\n");
}

static void stop(const char *l1, const char *l2)
{
    lcd_show(l1, l2);
    for (;;) {
    }
}

static void wait_jumpers_off(void)
{
    lcd_show("REMOVE JP1+JP2", "TO FINISH");
    while (line_jumper_fitted(LINE_RST845) || line_jumper_fitted(LINE_PGM845)) {
        hal_delay_ms(200);
    }
    lcd_show("JUMPERS OFF", "RESTARTING...");
    hal_delay_ms(1500);
}

int main(void)
{
    hal_init();
    lcd_init();
    lcd_show("CONVERTING UNIT", MIG_VERSION);
    hal_delay_ms(1500);

    /* 1. Own image */
    rep("Migration " MIG_VERSION ": old unit to the new bootloaders and firmware\r\n");
    bool self_ok = fw_image_valid(FW_SLOT_A_BASE);
    rep(self_ok ? "Image check: OK\r\n" : "Image check: FAILED\r\n");
    if (!self_ok) {
        stop("IMAGE DAMAGED", "COPY fw.hex AGAIN");
    }

    /* 2. What is on the LPC1768 */
    rep("Sector 0: ");
    rep(sector0_kind());
    rep("\r\n");
    rep(old_bootloader_present() ? "Old bootloader at 0xB000: present\r\n" : "Old bootloader at 0xB000: not there (converted part-way)\r\n");
    rep("Station ID (kept as it is): ");
    char stn[8];
    memcpy(stn, (const void *)0x1000, 7);
    stn[7] = '\0';
    rep(stn);
    rep("\r\n");

    /* 3. SD card */
    bool sd_ok = (f_mount(0, &s_fs) == FR_OK);
    if (sd_ok) {
        FRESULT m = f_mkdir("MIGRATE");
        sd_ok = (m == FR_OK || m == FR_EXIST);
    }
    if (sd_ok) {
        set_unit_dir();
        FRESULT m = f_mkdir(s_dir);
        sd_ok = (m == FR_OK || m == FR_EXIST);
        rep("Files for this unit: "); rep(s_dir); rep("\r\n");
    }
    if (!sd_ok) {
        stop("SD CARD FAIL", "CHECK FAT32 CARD");
    }
    if (!file_exists(unit_path("OLDFLASH.BIN"))) {
        lcd_show("SD CARD", "SAVING OLD FLASH");
        bool saved = write_file(unit_path("OLDFLASH.BIN"), (const void *)0x0, 0x18000);
        rep(saved ? "Old LPC1768 flash 0x0-0x17FFF saved: OLDFLASH.BIN\r\n" : "Old LPC1768 flash copy: FAILED\r\n");
        if (!saved) {
            stop("SD WRITE FAIL", "CHECK CARD");
        }
    } else {
        rep("OLDFLASH.BIN already there (rerun on this unit): kept\r\n");
    }

    /* 4. Station type: which production file. Before touching anything. */
    int type = m2_detect_type(note_line);
    rep("Station type: "); rep(m2_type_name(type)); rep("\r\n");
    if (type < 0) {
        write_file(unit_path("RESULT.TXT"), s_report, strlen(s_report));
        stop("UNKNOWN TYPE", "SEE RESULT.TXT");
    }

    /* 5. Jumpers */
    bool jp1 = line_jumper_fitted(LINE_RST845);
    bool jp2 = line_jumper_fitted(LINE_PGM845);
    rep(jp1 ? "JP1 (RST_845): fitted\r\n" : "JP1 (RST_845): open\r\n");
    rep(jp2 ? "JP2 (PGM_845): fitted\r\n" : "JP2 (PGM_845): open\r\n");
    if (!(jp1 && jp2)) {
        write_file(unit_path("RESULT.TXT"), s_report, strlen(s_report));
        stop("FIT JP1 + JP2", "THEN POWER ON");
    }

    /* 6. M1: the CTRL chip */
    static isp_survey_t isp;
    lcd_show("CTRL CHIP", "ROM LOADER...");
    bool isp_ok = isp_enter(&isp);
    char num[12];
    rep("LPC845 ROM loader: ");
    rep(isp_ok ? "OK, rails held by the RAM stub\r\n" : "FAILED\r\n");
    rep("  last step: "); fmt_dec(num, (uint32_t)isp.step); rep(num); rep("\r\n");
    m1_result_t m1 = M1_FAIL_READ;
    if (isp_ok) {
        isp_read_ids(&isp);
        rep("  J (part ID):   "); rep(isp.part_id); rep("\r\n");
        rep("M1, new CTRL "); rep(m1_ctrl_version()); rep(":\r\n");
        bool have_old = file_exists(unit_path("OLD845.BIN"));
        m1 = m1_program_ctrl(have_old ? NULL : save_old845, note_line);
        static const char *const names[] = {"DONE", "ALREADY NEW", "FAIL READ", "FAIL SECTOR 0", "FAIL ERASE", "FAIL WRITE", "FAIL VERIFY"};
        rep("  result: "); rep(names[m1]); rep("\r\n");
    } else {
        rep("  replies: "); rep(isp.note); rep("\r\n");
    }
    isp_leave();
    write_file(unit_path("RESULT.TXT"), s_report, strlen(s_report));
    if (!isp_ok) {
        stop("CTRL NO ANSWER", "POWER OFF/ON");
    }
    if (m1 == M1_FAIL_SECTOR0) {
        stop("CTRL SECTOR 0", "FAIL: CALL JIG");
    }
    if (m1 != M1_DONE && m1 != M1_ALREADY) {
        stop("CTRL FAILED", "POWER OFF/ON");
    }

    /* 7. M2: the main chip */
    rep("M2, main chip ("); rep(m2_type_name(type)); rep("):\r\n");
    m2_result_t m2 = m2_convert(type, note_line);
    static const char *const m2names[] = {"DONE", "FAIL FILE", "FAIL IMAGE", "FAIL FLASH", "FAIL BOOT SECTOR", "FAIL TRIAL"};
    rep("  result: "); rep(m2names[m2]); rep("\r\n");
    write_file(unit_path("RESULT.TXT"), s_report, strlen(s_report));
    switch (m2) {
    case M2_DONE:
        break;
    case M2_FAIL_FILE:
        stop("NO FIRMWARE FILE", "SEE RESULT.TXT");
    case M2_FAIL_IMAGE:
        stop("BAD FIRMWARE", "FILE: SEE RESULT");
    case M2_FAIL_BOOT_SECTOR:
        stop("BOOT SECTOR FAIL", "CALL JIG");
    case M2_FAIL_TRIAL:
        stop("NEW FIRMWARE", "FAILED TRIAL");
    default:
        stop("MAIN CHIP FAILED", "POWER OFF/ON");
    }

    /* 8. Jumpers off, then the new bootloader installs the production firmware */
    wait_jumpers_off();
    rep("Jumpers removed; restarting into the new bootloader\r\n");
    write_file(unit_path("RESULT.TXT"), s_report, strlen(s_report));
    NVIC_SystemReset();
    return 0;
}
