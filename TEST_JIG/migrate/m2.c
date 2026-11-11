/* M2: the LPC1768 to the new layout (LPC1768/boot/fw_layout.h), so the new bootloader installs the
 * production firmware through the normal update path (stage in slot B, PENDING, swap, trial).
 *
 * Sector 1 (settings) is not touched: the production firmware reads the station ID from offset 0,
 * where the old firmware keeps it, and checks its other fields against old bytes (flash_nvram.c).
 *
 * Order, so every power cut leaves a unit that boots into something that carries on:
 *   1. Production app from the SD card into slot B (0x38000), CRC and type checked. Old path intact.
 *   2. New bootloader into sectors 2-8 (old day data only). Old path intact.
 *   3. New boot stub into sector 0. THE unsafe moment (erase to write, well under 0.1 s). From here
 *      the new bootloader starts this image (slot A is valid, the journal holds no record yet).
 *   4. Sectors 9-16 erased (the old bootloader becomes error log, journal, swap); 25-29 blank.
 *   5. Journal: PENDING. Restart: the new bootloader swaps the production app in, on trial.
 * A run that finds the new boot stub already there carries on from step 4. */
#include "m2.h"
#include "iap1768.h"
#include "hal.h"
#include "ff.h"
#include "fw_layout.h"
#include "fw_journal.h"
#include "boot_core.h"
#include "boot1768_img.h"   /* build.sh: g_boot_stub[], g_bootloader[] from LPC1768/Debug */
#include <string.h>

#define BOOTLOADER_BASE 0x2000UL
#define BOOTLOADER_MAX  0x7000UL    /* sectors 2-8 */
#define PAGE            4096UL

static const char *const s_type_names[] = {"KSNDMC_TRG", "BIHAR_TRG", "KSNDMC_TWS"};
static const char *const s_type_files[] = {"fw_ksn_trg.hex", "fw_bihar.hex", "fw_ksn_tws.hex"};

static uint8_t s_page[PAGE] __attribute__((aligned(4)));

const char *m2_type_name(int type)
{
    return (type >= 0 && type <= 2) ? s_type_names[type] : "UNKNOWN";
}

static bool str_at(uint32_t addr, const char *want)
{
    return strncmp((const char *)addr, want, 16) == 0;
}

static bool contains(uint32_t addr, const char *want)
{
    char field[17];
    memcpy(field, (const void *)addr, 16);
    field[16] = '\0';
    return strstr(field, want) != NULL;
}

/* Old settings sector: agency at 0x1028, type at 0x1038 (read on unit 2114: "KSNDMC", "TRG").
 * MIGRATE/TYPE.TXT (KSNDMC_TRG, BIHAR_TRG or KSNDMC_TWS) overrides it. */
int m2_detect_type(m2_note_fn note)
{
    FIL f;
    if (f_open(&f, "MIGRATE/TYPE.TXT", FA_OPEN_EXISTING | FA_READ) == FR_OK) {
        char line[32] = "";
        f_gets(line, sizeof(line), &f);
        f_close(&f);
        for (int t = 0; t < 3; t++) {
            if (strncmp(line, s_type_names[t], strlen(s_type_names[t])) == 0) {
                note("type from MIGRATE/TYPE.TXT");
                return t;
            }
        }
        note("MIGRATE/TYPE.TXT not understood");
        return -1;
    }
    bool trg = str_at(0x1038, "TRG");
    bool tws = str_at(0x1038, "TWS") || str_at(0x1038, "AWS");
    if (str_at(0x1028, "KSNDMC") && trg) return 0;
    if (str_at(0x1028, "KSNDMC") && tws) return 2;
    if (contains(0x1028, "BIHAR") && trg) return 1;
    note("old settings: agency/type not recognised (MIGRATE/TYPE.TXT can name it)");
    return -1;
}

/* ---- 1. production app from an SD card hex file into slot B ---- */
static uint32_t s_page_addr;   /* Flash address of the page in s_page, 0 = none */

static bool flush_page(void)
{
    if (s_page_addr == 0) return true;
    bool ok = iap1768_program(s_page_addr, s_page, PAGE);
    s_page_addr = 0;
    return ok;
}

static bool put_byte(uint32_t slot_a_addr, uint8_t b)
{
    if (slot_a_addr < FW_SLOT_A_BASE || slot_a_addr >= FW_SLOT_A_BASE + FW_SLOT_SIZE) {
        return false;
    }
    uint32_t addr = slot_a_addr - FW_SLOT_A_BASE + FW_SLOT_B_BASE;
    uint32_t page = addr & ~(PAGE - 1UL);
    if (page != s_page_addr) {
        if (!flush_page()) return false;
        if (!iap1768_blank(page, PAGE)) return false;   /* Each page once, in order */
        memset(s_page, 0xFF, PAGE);
        s_page_addr = page;
    }
    s_page[addr - page] = b;
    return true;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static m2_result_t stage_app(int type, m2_note_fn note)
{
    FIL f;
    char line[80];
    if (f_open(&f, s_type_files[type], FA_OPEN_EXISTING | FA_READ) != FR_OK) {
        note("production file not on the card");
        return M2_FAIL_FILE;
    }
    lcd_show("MAIN CHIP", "STAGING APP");
    if (!iap1768_erase(FW_SLOT_B_SECTOR, FW_SLOT_B_SECTOR + FW_SLOT_SECTORS - 1UL)) {
        f_close(&f);
        return M2_FAIL_FLASH;
    }
    uint32_t base = 0;
    bool ended = false, ok = true;
    s_page_addr = 0;
    while (ok && !ended && f_gets(line, sizeof(line), &f)) {
        if (line[0] != ':') continue;
        uint8_t rec[40];
        int n = 0;
        for (int i = 1; hexval(line[i]) >= 0 && hexval(line[i + 1]) >= 0 && n < (int)sizeof(rec); i += 2) {
            rec[n++] = (uint8_t)(hexval(line[i]) * 16 + hexval(line[i + 1]));
        }
        uint8_t sum = 0;
        for (int i = 0; i < n; i++) sum += rec[i];
        if (n < 5 || n != rec[0] + 5 || sum != 0) { ok = false; note("bad record in the production file"); break; }
        uint32_t off = ((uint32_t)rec[1] << 8) | rec[2];
        switch (rec[3]) {
        case 0:
            for (int i = 0; i < rec[0] && ok; i++) ok = put_byte(base + off + (uint32_t)i, rec[4 + i]);
            if (!ok) note("data outside slot A, or out of order, in the production file");
            break;
        case 1: ended = true; break;
        case 2: base = (((uint32_t)rec[4] << 8) | rec[5]) << 4; break;
        case 4: base = (((uint32_t)rec[4] << 8) | rec[5]) << 16; break;
        default: break;   /* 03, 05: start addresses */
        }
    }
    f_close(&f);
    ok = ok && ended && flush_page();
    if (!ok) {
        if (!ended) note("production file has no end record");
        return M2_FAIL_FILE;
    }
    if (!fw_image_valid(FW_SLOT_B_BASE)) {
        note("staged image fails its CRC or image check");
        return M2_FAIL_IMAGE;
    }
    const uint32_t *v = (const uint32_t *)FW_SLOT_B_BASE;
    const fw_image_info_t *info = (const fw_image_info_t *)(v[FW_VECTOR_IMAGE_INFO] - FW_SLOT_A_BASE + FW_SLOT_B_BASE);
    if (info->profile != (uint32_t)type) {
        note("production file is built for another station type");
        return M2_FAIL_IMAGE;
    }
    note("production app staged in slot B, CRC good:");
    note(info->version);
    return M2_DONE;
}

/* ---- 2. and 3. ---- */
static bool write_region(uint32_t base, const uint8_t *data, uint32_t len)
{
    for (uint32_t off = 0; off < len; off += PAGE) {
        uint32_t n = (len - off < PAGE) ? (len - off) : PAGE;
        memset(s_page, 0xFF, PAGE);
        memcpy(s_page, data + off, n);
        if (!iap1768_program(base + off, s_page, PAGE)) return false;
    }
    return true;
}

static bool boot_stub_present(void)
{
    /* A plain loop: GCC may treat memcmp() on address 0 as undefined and drop it */
    const volatile uint8_t *flash = (const volatile uint8_t *)0x0;
    for (uint32_t i = 0; i < G_BOOT_STUB_LEN; i++) {
        if (flash[i] != g_boot_stub[i]) return false;
    }
    return true;
}

m2_result_t m2_convert(int type, m2_note_fn note)
{
    if (!boot_stub_present()) {
        /* 1. */
        m2_result_t r = stage_app(type, note);
        if (r != M2_DONE) return r;

        /* 2. */
        lcd_show("MAIN CHIP", "NEW BOOTLOADER");
        if (G_BOOTLOADER_LEN > BOOTLOADER_MAX ||
            !iap1768_erase(iap1768_sector_of(BOOTLOADER_BASE), iap1768_sector_of(BOOTLOADER_BASE + BOOTLOADER_MAX - 1UL)) ||
            !write_region(BOOTLOADER_BASE, g_bootloader, G_BOOTLOADER_LEN)) {
            note("bootloader write failed (old path still intact)");
            return M2_FAIL_FLASH;
        }
        note("new bootloader written and read back");

        /* 3. The unsafe moment */
        lcd_show("MAIN CHIP", "BOOT SECTOR");
        bool ok = iap1768_erase(0, 0);
        memset(s_page, 0xFF, 256);
        memcpy(s_page, g_boot_stub, G_BOOT_STUB_LEN);
        ok = ok && iap1768_program(0x0, s_page, 256);
        if (!ok) {
            note("BOOT SECTOR WRITE FAILED");
            return M2_FAIL_BOOT_SECTOR;
        }
        note("new boot stub written: the new bootloader starts the unit from now on");
    } else {
        fw_record_t rec;
        fw_journal_read(&rec);
        if (rec.type == FW_J_ROLLED_BACK) {
            note("the new firmware failed its trial and was rolled back");
            return M2_FAIL_TRIAL;
        }
        if (rec.type == FW_J_REJECTED || !fw_image_valid(FW_SLOT_B_BASE)) {
            m2_result_t r = stage_app(type, note);
            if (r != M2_DONE) return r;
        }
        note("new boot stub already there: carrying on");
    }

    /* 4. */
    lcd_show("MAIN CHIP", "CLEARING");
    fw_record_t rec;
    fw_journal_read(&rec);
    if (rec.type == FW_J_NONE) {
        if (!iap1768_blank(0x9000, 0x18000 - 0x9000) && !iap1768_erase(9, 16)) {
            note("erase of sectors 9-16 failed");
            return M2_FAIL_FLASH;
        }
        if (!iap1768_blank(0x9000, 0x18000 - 0x9000)) {
            note("sectors 9-16 not blank after erase");
            return M2_FAIL_FLASH;
        }
        /* Only on the first conversion (no journal record yet): on a converted unit this image
         * runs again only after a revert from the production menu, and the record log there holds
         * the station's stored records, which must stay. */
        if (!iap1768_blank(0x58000, 0x80000 - 0x58000)) {
            if (!iap1768_erase(25, 29) || !iap1768_blank(0x58000, 0x80000 - 0x58000)) {
                note("record log sectors 25-29 would not erase");
                return M2_FAIL_FLASH;
            }
        }
        note("error log, journal, swap and record log areas blank");
    } else {
        note("journal already in use (converted unit): error log and record log kept");
    }

    /* 5. */
    fw_journal_read(&rec);
    if (rec.type != FW_J_PENDING && !fw_journal_append(FW_J_PENDING, FW_OP_NONE, 0)) {
        note("journal write failed");
        return M2_FAIL_FLASH;
    }
    fw_journal_read(&rec);
    if (rec.type != FW_J_PENDING) {
        note("journal does not read back PENDING");
        return M2_FAIL_FLASH;
    }
    note("journal: PENDING. Restart installs the production firmware");
    return M2_DONE;
}
