#include "fw_app.h"
#include "fw_ctrl.h"
#include "hal_power.h"
#include "boot/fw_layout.h"
#include "boot/fw_journal.h"
#include "storage_log.h"
#include "flash_nvram.h"
#include "flash_log.h"
#include "intermcu_protocol.h"
#include "user_config.h"
#include "LPC17xx.h"
#include "errlog.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

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
#define TRIAL_HOLD_MAX_SEC   120  /* Sleep waits for confirmation at most this long after start */

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

/* Status of the last flash command that failed (0 = none yet), for the log: 4 = source not mapped,
 * 9 = sector not prepared, and so on (UM10360 IAP status codes) */
uint32_t fw_hal_last_status = 0;

/* Interrupts stay off during flash commands: code can't run from flash meanwhile */
static bool iap_run(void)
{
    __disable_irq();
    iap_call(s_cmd, s_res);
    __enable_irq();
    if (s_res[0] != 0) {
        fw_hal_last_status = s_res[0];
    }
    return s_res[0] == 0;
}

/* The chip's flash write takes its data from main RAM here. Task stacks are in the heap in AHB SRAM,
 * and from v6.9.15 (20 KB heap) a task made later sits across or above 0x20080000, a source the boot
 * ROM refused: v6.9.15 could not stage an update ("UPDATE STOPPED FLASH ERROR", 1922, 2026-11-08). */
static uint32_t s_iap_src[64];

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
    if (len > sizeof(s_iap_src)) {
        return false;
    }
    memcpy(s_iap_src, src, len);
    src = s_iap_src;
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

    /* Before the main loop can start a slot. From fw_app_step alone, a slot wake ran its slot at +1 s
     * and slept before the check came round, so it copied the old type's records to the card under
     * the new name and would have sent its queue to the new server (1922, 17:30, 2026-11-09). Firmware
     * on trial still waits for its confirmation, so a failed update costs no data. */
    if (!s_on_trial) {
        check_station_type_change();
    }
}

/* ---- The LPC845's side of an update ---- */
#define CTRL_START_SEC      10  /* The LPC845's watchdog gives firmware on trial at least 3.5 minutes */
#define CTRL_MIN_FRAMES     5
#define CTRL_MAX_TRIES      12

void fw_ctrl_tx(const char *frame)
{
    hal_power_send_frame(frame);
}

bool fw_ctrl_wait_reply(char kind, char *reply, size_t size, uint32_t timeout_ms)
{
    portTickType start = xTaskGetTickCount();
    portTickType limit = timeout_ms / portTICK_RATE_MS;
    /* Anything still sitting there answers an earlier command */
    while ((xTaskGetTickCount() - start) < limit) {
        if (intermcu_take_u_reply(reply, size) && reply[2] == kind) {
            return true;
        }
        vTaskDelay(5 / portTICK_RATE_MS);
    }
    return false;
}

static const char *s_ctrl_last_result = NULL;
static bool s_ctrl_state_known = false;
static char s_845_version[sizeof(((fw_ctrl_state_t *)0)->version)] = "";

const char *fw_app_845_version(void)
{
    return s_845_version;
}

const char *fw_app_ctrl_last_result(void)
{
    return s_ctrl_last_result;
}

static void ctrl_report(unsigned state, const char *version)
{
    const char *what;
    bool failed = true;
    switch (state) {
    case FW_J_CONFIRMED:   what = "CTRL UPDATE OK"; failed = false; break;
    case FW_J_ROLLED_BACK: what = "CTRL FAILED:OLD"; break;
    case FW_J_RECOVERED:   what = "CTRL RESTORED"; break;
    case FW_J_REJECTED:    what = "CTRL REJECTED"; break;
    default:               return;
    }
    s_ctrl_last_result = what;

    char line[64];
    if (version && version[0]) {
        strncpy(s_845_version, version, sizeof(s_845_version) - 1);
        s_845_version[sizeof(s_845_version) - 1] = '\0';
    }
    snprintf(line, sizeof(line), "%s, now running %s", what, version);
    storage_log_debug(failed ? "ERROR" : "FW", line);
    char history[80];
    snprintf(history, sizeof(history), "Station %s: %s", flash_nvram_get_station_id(), line);
    storage_append_stamped(FW_HISTORY_FILENAME, history);
    fw_ctrl_tx("$UK;\n");
}

/* Once a second, never waiting: this task runs the slots. New LPC845 firmware counts as healthy once
 * this end has heard good frames from it. An LPC845 with no bootloader never answers, which is fine. */
static void ctrl_step(uint32_t uptime_sec, uint32_t good_frames)
{
    static uint8_t s_tries = 0;
    static bool s_done = false;
    char reply[40];
    fw_ctrl_state_t state;

    if (s_done || uptime_sec < CTRL_START_SEC || good_frames < CTRL_MIN_FRAMES || intermcu_get_845_system() < 0) {
        return;
    }
    if (intermcu_take_u_reply(reply, sizeof(reply)) && fw_ctrl_parse_state(reply, &state)) {
        if (!s_ctrl_state_known) {
            char line[48];
            s_ctrl_state_known = true;
            strncpy(s_845_version, state.version, sizeof(s_845_version) - 1);
            s_845_version[sizeof(s_845_version) - 1] = '\0';
            snprintf(line, sizeof(line), "LPC845 running %s", state.version);
            storage_log_debug("FW", line);
        }
        if (state.state == FW_J_TRIAL_BOOT) {
            storage_log_debug("FW", "New LPC845 firmware confirmed healthy");
            fw_ctrl_tx("$UC;\n");
            return;
        }
        ctrl_report(state.state, state.version);
        s_done = true;
        return;
    }
    if (s_tries >= CTRL_MAX_TRIES) {
        s_done = true;
        return;
    }
    s_tries++;
    fw_ctrl_tx("$UQ;\n");
}

/* New firmware not yet confirmed: power must stay on until it is (about 30 s after start), or the boot
 * counts as a failed trial and three of those in a row put the old firmware back */
bool fw_app_trial_unconfirmed(void)
{
    /* Never longer than 2 minutes: if confirming can't happen (no LPC845 frames, journal write
     * failing), holding power on would only run the battery down */
    return s_on_trial && !s_confirmed && (xTaskGetTickCount() / configTICK_RATE_HZ) < TRIAL_HOLD_MAX_SEC;
}

void fw_app_step(uint32_t uptime_sec, uint32_t good_frames)
{
    ctrl_step(uptime_sec, good_frames);

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

#define WD_RUN_TICKS 60000000UL  /* 60 s: the watchdog runs from the 4 MHz IRC, divided by 4 */

static const char *const s_wd_names[WD_COUNT] = { "Main", "Ui", "Modem" };
static const uint16_t s_wd_limit_sec[WD_COUNT] = { 360, 60, 180 };
static xTaskHandle s_wd_tasks[WD_COUNT];
static volatile portTickType s_wd_seen[WD_COUNT];
static bool s_wd_running = false;

void wd_register(wd_task_t id, void *task_handle)
{
    s_wd_seen[id] = xTaskGetTickCount();
    s_wd_tasks[id] = (xTaskHandle)task_handle;
}

void wd_checkin(void)
{
    xTaskHandle me = xTaskGetCurrentTaskHandle();
    for (int i = 0; i < WD_COUNT; i++) {
        if (s_wd_tasks[i] == me) {
            s_wd_seen[i] = xTaskGetTickCount();
        }
    }
}

void fw_app_watchdog_step(void)
{
    /* New firmware on trial: the bootloader's watchdog, left unfed, restarts it if it hangs */
    if (s_on_trial && !s_confirmed) {
        return;
    }

    portTickType now = xTaskGetTickCount();
    for (int i = 0; i < WD_COUNT; i++) {
        if (s_wd_tasks[i] && (now - s_wd_seen[i]) > (portTickType)s_wd_limit_sec[i] * configTICK_RATE_HZ) {
            char msg[48];
            snprintf(msg, sizeof(msg), "Task %s stopped responding: restarting", s_wd_names[i]);
            storage_log_debug("ERROR", msg);
            errlog_step();  /* In flash before the restart */
            LPC_RTC->GPREG1 = (uint32_t)i;
            LPC_RTC->GPREG0 = STUCK_MAGIC;
            NVIC_SystemReset();
        }
    }

    /* Can't be turned off once started. The disk task feeds it, so it also catches that task stopping. */
    taskENTER_CRITICAL();
    if (!s_wd_running) {
        LPC_WDT->WDCLKSEL = 0;
        LPC_WDT->WDTC = WD_RUN_TICKS;
        LPC_WDT->WDMOD = 3; /* Enable, reset on timeout */
        s_wd_running = true;
    }
    LPC_WDT->WDFEED = 0xAA;
    LPC_WDT->WDFEED = 0x55;
    taskEXIT_CRITICAL();
}
