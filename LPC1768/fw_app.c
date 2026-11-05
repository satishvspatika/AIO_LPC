#include "fw_app.h"
#include "boot/fw_layout.h"
#include "boot/fw_journal.h"
#include "storage_log.h"
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

void fw_app_init(void)
{
    fw_record_t rec;
    fw_journal_read(&rec);
    s_on_trial = (rec.type == FW_J_TRIAL_BOOT);

    char line[64];
    const char *what;
    switch (rec.type) {
    case FW_J_TRIAL_BOOT:   what = "new firmware on trial"; break;
    case FW_J_CONFIRMED:    what = "last update confirmed"; break;
    case FW_J_ROLLED_BACK:  what = "UPDATE FAILED, previous firmware restored"; break;
    case FW_J_RECOVERED:    what = "FIRMWARE WAS DAMAGED, restored from the other copy"; break;
    case FW_J_REJECTED:     what = "UPDATE REJECTED, staged image was invalid"; break;
    default:                what = "no update history"; break;
    }
    snprintf(line, sizeof(line), "%s: %s", g_fw_image_info.version, what);
    storage_log_debug((rec.type == FW_J_ROLLED_BACK || rec.type == FW_J_RECOVERED || rec.type == FW_J_REJECTED) ? "ERROR" : "FW", line);
}

void fw_app_step(uint32_t uptime_sec, uint32_t good_frames)
{
    if (!s_on_trial || s_confirmed || uptime_sec < CONFIRM_AFTER_SEC || good_frames < CONFIRM_MIN_FRAMES) {
        return;
    }
    if (fw_journal_append(FW_J_CONFIRMED, FW_OP_NONE, 0)) {
        s_confirmed = true;
        storage_log_debug("FW", "New firmware confirmed healthy");
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
