#include "fw_app.h"
#include "boot/fw_layout.h"
#include "boot/fw_journal.h"
#include "storage_log.h"
#include "flash_nvram.h"
#include "flash_log.h"
#include "intermcu_protocol.h"
#include "user_config.h"
#include "LPC17xx.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

/* Reserved vector 8 points here. scripts/fw_image.py fills in length and crc32 after linking. */
__attribute__((used))
const fw_image_info_t g_fw_image_info = {
    .magic = FW_IMAGE_MAGIC,
    .board = FW_BOARD_ID,
    .length = 0,
    .crc32 = 0,
    .profile = UNIT_CFG,
    .version = FIRMWARE_VERSION
};

#define IAP_LOCATION    0x1FFF1FF1UL
#define CONFIRM_AFTER_SEC    30
#define CONFIRM_MIN_FRAMES   5

typedef void (*iap_entry_t)(unsigned long[], unsigned long[]);
static const iap_entry_t iap_call = (iap_entry_t)IAP_LOCATION;
static const unsigned long s_cclk_khz[] = { 100000, 398000 };
static unsigned long s_cmd[5];
static unsigned long s_res[5];
static bool s_on_trial = false;
static volatile bool s_confirmed = false;
static const char *s_last_result = NULL;
static uint8_t s_outcome_pending = FW_J_NONE;
static bool s_outcome_done = false;
#define OUTCOME_MIN_FRAMES  3
#define OUTCOME_LATEST_SEC  15

const uint8_t *fw_hal_ptr(uint32_t addr)
{
    return (const uint8_t *)addr;
}

/* Interrupts stay off during flash commands: code can't run from flash meanwhile */
static bool iap_run(void)
{
    __disable_irq();
    iap_call(s_cmd, s_res);
    __enable_irq();
    return s_res[0] == 0;
}

static uint32_t sector_of(uint32_t addr)
{
    return (addr < 0x10000UL) ? (addr / 0x1000UL) : (16UL + (addr - 0x10000UL) / 0x8000UL);
}

static bool iap_prepare(uint32_t sector)
{
    s_cmd[0] = 50;
    s_cmd[1] = sector;
    s_cmd[2] = sector;
    return iap_run();
}

bool fw_hal_erase(uint32_t sector)
{
    for (unsigned c = 0; c < sizeof(s_cclk_khz) / sizeof(s_cclk_khz[0]); c++) {
        if (!iap_prepare(sector)) {
            continue;
        }
        s_cmd[0] = 52;
        s_cmd[1] = sector;
        s_cmd[2] = sector;
        s_cmd[3] = s_cclk_khz[c];
        if (iap_run()) {
            return true;
        }
    }
    return false;
}

bool fw_hal_program(uint32_t addr, const void *src, uint32_t len)
{
    for (unsigned c = 0; c < sizeof(s_cclk_khz) / sizeof(s_cclk_khz[0]); c++) {
        if (!iap_prepare(sector_of(addr))) {
            continue;
        }
        s_cmd[0] = 51;
        s_cmd[1] = addr;
        s_cmd[2] = (unsigned long)src;
        s_cmd[3] = len;
        s_cmd[4] = s_cclk_khz[c];
        if (iap_run()) {
            return true;
        }
    }
    return false;
}

uint32_t fw_app_running_crc(void)
{
    /* Read from flash: the build stamps this after compiling, so the compiler's copy is wrong */
    return *(const volatile uint32_t *)&g_fw_image_info.crc32;
}

const char *fw_app_last_result(void)
{
    return s_last_result;
}

/* Once for each update: writes down how it ended. A power cut before REPORTED just repeats this.
 * The file stays on the card, as one card serves many stations: only the menu can install it. */
static void report_outcome(uint8_t type)
{
    const char *what;
    bool failed = true;
    switch (type) {
    case FW_J_CONFIRMED:    what = "UPDATE OK"; failed = false; break;
    case FW_J_ROLLED_BACK:  what = "FAILED: OLD F/W"; break;
    case FW_J_RECOVERED:    what = "F/W RESTORED"; break;
    case FW_J_REJECTED:     what = "REJECTED"; break;
    case FW_J_REVERTED:     what = "REVERTED"; failed = false; break;
    default:                return;
    }
    s_last_result = what;

    char line[64];
    snprintf(line, sizeof(line), "%s, now running %s", what, g_fw_image_info.version);
    storage_log_debug(failed ? "ERROR" : "FW", line);
    /* One card serves many stations */
    char history[80];
    snprintf(history, sizeof(history), "Station %s: %s", flash_nvram_get_station_id(), line);
    storage_append_stamped(FW_HISTORY_FILENAME, history);
    fw_journal_append(FW_J_REPORTED, FW_OP_NONE, 0);
}

/* A new station type starts fresh, as the operator was warned: queued records are in the old
 * server's format, and the old totals mean nothing to the new type. Done once the new firmware has
 * confirmed itself, so a failed update costs no data, and from every boot's check, so a power cut
 * can't skip it and an MCU-Link flash counts too. */
static void check_station_type_change(void)
{
    int last = flash_nvram_get_profile();
    if (last == UNIT_CFG) {
        return;
    }
    if (last >= 0) {
        char line[80];
        int removed = storage_erase_station_data();
        storage_backlog_clear_all();
        flash_log_erase_all();
        intermcu_reset_cumulative_rain();
        flash_nvram_note_last_logged("NO DATA YET");
        snprintf(line, sizeof(line), "Station %s type %d to %d: all data erased, %d SD files",
                 flash_nvram_get_station_id(), last, UNIT_CFG, removed);
        storage_log_debug("FW", line);
        storage_append_stamped(FW_HISTORY_FILENAME, line);
    }
    /* Last: until this is written, the erase runs again after a power cut */
    flash_nvram_set_profile(UNIT_CFG);
}

void fw_app_init(void)
{
    fw_record_t rec;
    fw_journal_read(&rec);
    s_on_trial = (rec.type == FW_J_TRIAL_BOOT);

    s_outcome_pending = rec.type;

    char line[48];
    snprintf(line, sizeof(line), "Running %s%s", g_fw_image_info.version, s_on_trial ? ", new firmware on trial" : "");
    storage_log_debug("FW", line);
}

void fw_app_step(uint32_t uptime_sec, uint32_t good_frames)
{
    /* A few seconds in, so the clock has arrived and the history gets a real time */
    if (!s_outcome_done && (good_frames >= OUTCOME_MIN_FRAMES || uptime_sec >= OUTCOME_LATEST_SEC)) {
        s_outcome_done = true;
        if (!s_on_trial) {
            check_station_type_change();
        }
        report_outcome(s_outcome_pending);
    }
    if (!s_on_trial || s_confirmed || uptime_sec < CONFIRM_AFTER_SEC || good_frames < CONFIRM_MIN_FRAMES) {
        return;
    }
    if (fw_journal_append(FW_J_CONFIRMED, FW_OP_NONE, 0)) {
        s_confirmed = true;
        storage_log_debug("FW", "New firmware confirmed healthy");
        check_station_type_change();
        report_outcome(FW_J_CONFIRMED);
    } else {
        storage_log_debug("ERROR", "Could not record firmware confirmation, will retry");
    }
}

void fw_app_watchdog_step(void)
{
    /* It can't be turned off once started, so a confirmed image keeps it fed until the next reset */
    if (s_confirmed && (LPC_WDT->WDMOD & 1)) {
        taskENTER_CRITICAL();
        LPC_WDT->WDFEED = 0xAA;
        LPC_WDT->WDFEED = 0x55;
        taskEXIT_CRITICAL();
    }
}
