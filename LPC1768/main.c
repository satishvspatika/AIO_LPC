#include "FreeRTOS.h"
#include "task.h"
#include "hal_power.h"
#include "intermcu_protocol.h"
#include "telemetry_formatter.h"
#include "sim7600_driver.h"
#include "ui_display.h"
#include "storage_log.h"
#include "fw_app.h"
#include "flash_nvram.h"
#include "flash_log.h"
#include "user_config.h"
#include "lpc17xx_uart.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* --- Crash Recovery ---
 * A fault must never leave the unit frozen: that would lose every later 15-minute slot. Save
 * where it happened in the RTC registers (they survive a reset), restart, and log it at boot. */
#define CRASH_MAGIC 0xDEADC0DEUL

__attribute__((used)) void hard_fault_capture(const uint32_t *frame)
{
    uint32_t pc = 0, lr = 0;
    uint32_t addr = (uint32_t)frame;

    /* The stack pointer itself may be what went wrong */
    if (addr >= 0x10000000UL && addr <= (0x10008000UL - 32UL)) {
        lr = frame[5];
        pc = frame[6];
    }
    LPC_RTC->GPREG1 = pc;
    LPC_RTC->GPREG2 = lr;
    LPC_RTC->GPREG3 = SCB->CFSR;
    LPC_RTC->GPREG0 = CRASH_MAGIC;
    NVIC_SystemReset();
}

/* Replaces the endless loop in the startup file */
__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile(
        "tst lr, #4            \n"
        "ite eq                \n"
        "mrseq r0, msp         \n"
        "mrsne r0, psp         \n"
        "b hard_fault_capture  \n"
    );
}

static void log_previous_crash(void)
{
    if (LPC_RTC->GPREG0 == CRASH_MAGIC) {
        char msg[80];
        snprintf(msg, sizeof(msg), "RESTARTED AFTER CRASH: pc=0x%08lX lr=0x%08lX cfsr=0x%08lX",
                 (unsigned long)LPC_RTC->GPREG1, (unsigned long)LPC_RTC->GPREG2, (unsigned long)LPC_RTC->GPREG3);
        storage_log_debug("ERROR", msg);
        LPC_RTC->GPREG0 = 0;
    }
}

static void vDisk_Task(void *pvParameters)
{
    (void)pvParameters;
    portTickType xLastWakeTime = xTaskGetTickCount();

    uint32_t tick = 0;

    for (;;) {
        storage_log_timer_step_10ms();
        storage_log_drain_step();
        if (++tick >= 100) {
            tick = 0;
            fw_app_watchdog_step();
        }
        vTaskDelayUntil(&xLastWakeTime, 10 / portTICK_RATE_MS);
    }
}

static void vUi_Task(void *pvParameters)
{
    (void)pvParameters;
    portTickType xLastWakeTime = xTaskGetTickCount();
    uint8_t sec_accum = 0;

    for (;;) {
        if (++sec_accum >= 20) {
            sec_accum = 0;
            intermcu_tick_second();
        }

        telemetry_data_t telem;
        memset(&telem, 0, sizeof(telem));
        intermcu_get_telemetry(&telem);

        modem_status_t modem_stat;
        sim7600_get_status(&modem_stat);

        ui_display_task_step(&telem, &modem_stat);

        vTaskDelayUntil(&xLastWakeTime, 50 / portTICK_RATE_MS);
    }
}

/* --- 15-Minute Slot Scheduler ---
 * Every 15-minute slot has a unique, ever-increasing ID, so a slot still fires if the
 * exact boundary minute was missed (late boot, modem busy, previous send overran).
 * Every slot must end in one of: delivered, saved to backlog, or logged as lost. */
#define SLOT_MODEM_WAIT_SEC     120 /* How long to wait for SMS/supervisor to free the modem */
#define SLOT_MODEM_POWER_TRIES  2   /* Modem boot + network registration attempts */
#define NO_NETWORK_SLOTS_BEFORE_ONE_TRY 4   /* About an hour with no network: one try per slot */
#define SLOT_SEND_BUDGET_SEC    300 /* No new send starts past this, so bad signal can't drain the battery */
#define REJECTIONS_ALLOWED      3   /* Separate slots, as a wrong clock can cause one and gets corrected */
#define SLOT_CLOCK_WAIT_SEC     180 /* Scheduled wake with no valid clock: give up after this */
#define SLOT_CLOCK_NO_NET_SEC   120 /* Sooner if there's no network to get the time from */

typedef enum {
    SLOT_SENT = 0,
    SLOT_BACKLOG,
    SLOT_LOST
} slot_result_t;

static volatile int32_t s_last_slot_id = -1;
static volatile bool s_slot_armed = false;
static volatile bool s_waiting_for_clock = false;
static uint32_t s_reset_reason = 0;
static char s_slot_line[160]; /* Used by vMain_Task only */

/* Parses YYYYMMDDHHMMSS. Slot ID = days since year 0 * 96 + slot of the day. */
static bool parse_slot_clock(const telemetry_data_t *t, int *out_min, int *out_sec, int32_t *out_id)
{
    const char *p = t->present_time;

    if (strlen(p) < 14 || strncmp(p, "20", 2) != 0) return false;
    for (int i = 0; i < 14; i++) {
        if (p[i] < '0' || p[i] > '9') return false;
    }

    int yr = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
    int mo = (p[4]-'0')*10 + (p[5]-'0');
    int da = (p[6]-'0')*10 + (p[7]-'0');
    int hr = (p[8]-'0')*10 + (p[9]-'0');
    int mi = (p[10]-'0')*10 + (p[11]-'0');
    int se = (p[12]-'0')*10 + (p[13]-'0');

    if (yr < 2024 || mo < 1 || mo > 12 || da < 1 || da > 31 || hr > 23 || mi > 59 || se > 59) {
        return false;
    }

    *out_min = mi;
    *out_sec = se;
    *out_id = intermcu_day_number(yr, mo, da) * 96 + (hr * 60 + mi) / 15;
    return true;
}

/* True if powering down now could lose a slot. The window before the boundary covers a
 * power-off command crossing with the LPC845's own slot wake, which it would undo. */
bool slot_scheduler_is_due(void)
{
    telemetry_data_t t;
    int min = 0, sec = 0;
    int32_t id = 0;

    if (!intermcu_get_telemetry(&t) || !parse_slot_clock(&t, &min, &sec, &id)) {
        return false;
    }
    if ((min % 15) == 14 && sec >= 55) {
        return true;
    }
    if (!s_slot_armed) {
        return (min % 15) == 0;
    }
    return id > s_last_slot_id;
}

static void format_slot_payload(char *payload, size_t size, const telemetry_data_t *telem, int csq_dbm)
{
    const char *stn_id = flash_nvram_get_station_id();

    #if (UNIT_CFG == KSNDMC_TRG)
    format_payload_ksndmc_trg(payload, size, stn_id, telem, csq_dbm);
    #elif (UNIT_CFG == BIHAR_TRG)
    format_payload_bihar_trg(payload, size, stn_id, telem, csq_dbm);
    #else
    format_payload_ksndmc_tws(payload, size, stn_id, telem, csq_dbm);
    #endif
}

static const char *slot_timestamp(const telemetry_data_t *telem)
{
    return (strlen(telem->slot_time) >= 8 && strncmp(telem->slot_time, "20", 2) == 0 && telem->slot_time[2] != '0')
           ? telem->slot_time : telem->present_time;
}

static int32_t s_archive_slot_id = 0;
static uint32_t s_last_record_seq = 0;

/* SD file for a record: DATA/<station>_<YYYYMMDD>.TXT, one per rain day (08:45 to 08:30 next
 * day) named by the date it closes on. The record text starts NN,YYYY-MM-DD,HH:MM */
static bool write_record_to_sd(const char *record)
{
    char ts[16];
    char close_date[12];
    char station[8];
    char day_file[24];

    if (strlen(record) < 19) {
        return false;
    }
    snprintf(ts, sizeof(ts), "%.4s%.2s%.2s%.2s%.2s00", record + 3, record + 8, record + 11, record + 14, record + 17);
    if (!intermcu_rain_close_date(ts, close_date, sizeof(close_date))) {
        return false;
    }
    format_station_id(station, sizeof(station), flash_nvram_get_station_id());
    snprintf(day_file, sizeof(day_file), "%s_%s", station, close_date);
    return storage_archive_slot(day_file, record);
}

/* Copies records the SD card doesn't have yet from flash. If there was no card, or a new one
 * is fitted, this catches it up. A limited number per slot keeps sending on time. */
#define SD_SYNC_MAX_RECORDS 96

static void sync_flash_records_to_sd(void)
{
    static bool s_sd_ok = true;
    char line[16];
    uint32_t synced = 0;

    if (storage_state_load(FLASH_SYNC_FILENAME, line, sizeof(line)) && strlen(line) >= 10) {
        synced = (uint32_t)atol(line);
    }
    if (synced > flash_log_newest_seq()) {
        synced = 0; /* Flash was erased or reflashed since this was saved */
    }

    uint32_t start = synced;
    bool ok = true;
    for (int n = 0; n < SD_SYNC_MAX_RECORDS; n++) {
        const flash_log_page_t *rec = flash_log_next_record(synced);
        if (!rec) {
            break;
        }
        if (!write_record_to_sd(rec->record)) {
            ok = false;
            break;
        }
        synced = rec->seq;
        hal_power_heartbeat();
    }

    if (synced != start) {
        snprintf(line, sizeof(line), "%010lu\r\n", (unsigned long)synced);
        storage_state_save(FLASH_SYNC_FILENAME, line);
        snprintf(s_slot_line, sizeof(s_slot_line), "SD card updated from flash up to record %lu", (unsigned long)synced);
        storage_log_debug("SDC", s_slot_line);
    }
    if (!ok) {
        if (s_sd_ok) {
            storage_log_debug("ERROR", "SD card not available: records are being kept in flash");
        }
        storage_sd_is_ok(); /* Prepares a refitted card for the next slot */
    }
    s_sd_ok = ok;
}

/* Keeps the record: flash always, and the SD card when there is one */
static void archive_slot(const telemetry_data_t *telem, int csq_dbm, const char *payload)
{
    /* A restart mid-slot runs the slot again. Its record is already kept, so don't store a second
     * row. It's still sent again: flash can't show whether the first send got through. */
    const flash_log_page_t *newest = flash_log_newest_record();
    if (newest && newest->slot_id == s_archive_slot_id) {
        s_last_record_seq = newest->seq;
        storage_log_debug("SLOT", "This slot is already stored: sending it again without a second copy");
        sync_flash_records_to_sd();
        return;
    }

    char record[FLASH_LOG_RECORD_SIZE];
    format_sd_record(record, sizeof(record), telem, csq_dbm);

    s_last_record_seq = flash_log_append(s_archive_slot_id, record, payload);
    if (s_last_record_seq != 0) {
        sync_flash_records_to_sd();
    } else {
        storage_log_debug("ERROR", "Could not write record to flash, trying SD card only");
        if (!write_record_to_sd(record)) {
            storage_log_debug("ERROR", "RECORD NOT STORED: flash and SD card both failed");
        }
    }
}

static slot_result_t save_slot_for_retry(const telemetry_data_t *telem, const char *payload)
{
    slot_result_t result = SLOT_BACKLOG;

    flash_log_note_delivery(s_last_record_seq, false);

    if (strlen(payload) >= 384) {
        storage_log_debug("ERROR", "Slot payload exceeds 383 bytes: backlog copy will be truncated");
    }
    if (!storage_backlog_save(telem->slot_time, payload)) {
        /* No usable SD card: the record is already in flash, so queue it there */
        if (s_last_record_seq != 0 && flash_log_mark_pending(s_last_record_seq)) {
            ui_inc_backlog_count();
            storage_log_debug("BACKLOG", "SD backlog unavailable, record queued in flash");
        } else {
            storage_log_debug("ERROR", "SLOT DATA NOT QUEUED: SD backlog and flash queue both failed");
            result = SLOT_LOST;
        }
    }
    return result;
}

/* Power: GPS only runs in a slot's own modem time, and at set slots, so nothing needs storing to
 * count tries. No position: 01:00, 07:00, 13:00 and 19:00. Position saved: one check at 13:00. */
static void __attribute__((noinline)) gps_auto_step(const telemetry_data_t *telem)
{
    int slot_of_day = (int)(s_archive_slot_id % 96);
    bool have_fix = flash_nvram_get_location(NULL, NULL);
    if (have_fix ? (slot_of_day != 52) : ((slot_of_day % 24) != 4)) {
        return;
    }
    if (telem->bat1_volt < GPS_MIN_BATTERY_V) {
        storage_log_debug("GPS", "Battery low: scheduled search skipped");
        return;
    }
    char today[12];
    snprintf(today, sizeof(today), "%.4s-%.2s-%.2s", telem->present_time, telem->present_time + 4, telem->present_time + 6);
    hal_power_heartbeat();
    sim7600_gps_update(today);
}

static slot_result_t execute_slot_transmission(telemetry_data_t *telem, int *out_attempts, int *out_csq, const char **out_reason)
{
    char payload[512] = "";
    slot_result_t result;

    *out_attempts = 0;
    *out_csq = -113;

    /* An SMS or the modem supervisor may hold the modem: wait instead of dropping the slot */
    bool acquired = false;
    for (int waited = 0; waited < SLOT_MODEM_WAIT_SEC && !acquired; waited += 5) {
        acquired = sim7600_acquire(5000);
        if (!acquired) {
            storage_log_debug("MODEM", "Modem busy, slot transmission waiting...");
        }
    }
    if (!acquired) {
        storage_log_debug("ERROR", "Modem stayed busy for 120s: saving slot to backlog");
        format_slot_payload(payload, sizeof(payload), telem, -113);
        archive_slot(telem, -113, payload);
        *out_reason = "MODEM_BUSY";
        return save_slot_for_retry(telem, payload);
    }

    /* A cancelled SMS must not abort this slot's modem power-on */
    sim7600_clear_cancel();

    storage_log_debug("MODEM", "SIM7600 slot transmission starting...");

    /* Worked out from the records, so it survives the power-off between slots with no extra writes */
    const portTickType budget_end = xTaskGetTickCount() + (SLOT_SEND_BUDGET_SEC * 1000 / portTICK_RATE_MS);
    #define SLOT_BUDGET_LEFT() ((long)(budget_end - xTaskGetTickCount()) > 0)
    const int power_tries = (flash_log_no_network_run(NO_NETWORK_SLOTS_BEFORE_ONE_TRY) >= NO_NETWORK_SLOTS_BEFORE_ONE_TRY)
                            ? 1 : SLOT_MODEM_POWER_TRIES;

    bool modem_up = false;
    for (int attempt = 1; attempt <= power_tries && !modem_up && SLOT_BUDGET_LEFT(); attempt++) {
        hal_power_heartbeat();
        modem_up = sim7600_power_on();
        if (!modem_up) {
            snprintf(s_slot_line, sizeof(s_slot_line), "Modem power-on/registration failed (try %d/%d)", attempt, SLOT_MODEM_POWER_TRIES);
            storage_log_debug("MODEM", s_slot_line);
            /* A second try can't find a SIM that isn't there. Each slot still checks afresh. */
            if (sim7600_sim_missing()) {
                break;
            }
            if (attempt < power_tries) {
                sim7600_power_off();
            }
        }
    }

    if (modem_up) {
        modem_status_t status;
        sim7600_get_status(&status);
        *out_csq = status.csq_dbm;

        /* Format HTTP payload based on active configuration */
        format_slot_payload(payload, sizeof(payload), telem, status.csq_dbm);

        /* ESP32 flow: the record is on the card before any attempt to send it */
        archive_slot(telem, status.csq_dbm, payload);

        const http_endpoint_t *endpoint = telemetry_get_endpoint(UNIT_CFG);

        snprintf(s_slot_line, sizeof(s_slot_line), "Endpoint: %s:%d%s", endpoint->ip, endpoint->port, endpoint->path);
        storage_log_debug("HTTP", s_slot_line);
        storage_log_debug("HTTP_POST", payload);

        /* This slot's record: 3 tries in all, one by each route. The route that worked
         * then carries the backlog. */
        bool post_ok = false;
        int good_route = 0;
        const char *fail_reason = "HTTP_FAILED";
        for (int attempt = 1; attempt <= 3; attempt++) {
            char att_msg[48];
            snprintf(att_msg, sizeof(att_msg), "HTTP Post Attempt %d/3...", attempt);
            storage_log_debug("HTTP", att_msg);
            *out_attempts = attempt;
            hal_power_heartbeat();

            post_ok = sim7600_http_post_once(endpoint, payload, attempt - 1);
            if (post_ok) {
                good_route = attempt - 1;
                storage_log_debug("HTTP", "Transmission succeeded (200 OK)!");
                break;
            }
            if (sim7600_post_unanswered()) {
                storage_log_debug("HTTP", "Sent with no answer: keeping it for the next slot, no repeat now");
                break;
            }
            /* Signal gone since registering, or too long already: it's saved below either way */
            if (!SLOT_BUDGET_LEFT()) {
                storage_log_debug("HTTP", "Slot sending time used up: keeping it for the next slot");
                fail_reason = "TIME_LIMIT";
                break;
            }
            if (!sim7600_post_rejected() && !sim7600_still_registered()) {
                storage_log_debug("HTTP", "Network lost after registering: keeping it for the next slot");
                fail_reason = "NET_LOST";
                break;
            }
            if (attempt < 3) {
                storage_log_debug("HTTP", "Attempt failed, waiting 3s before the next try...");
                vTaskDelay(3000 / portTICK_RATE_MS);
            }
        }

        if (post_ok) {
            result = SLOT_SENT;
            /* In SLOTS.CSV, so a card shows at a glance which route each slot used */
            static const char *const ok_reasons[] = { "OK_NAME", "OK_IP", "OK_MODEM_HTTP" };
            *out_reason = ok_reasons[good_route];

            /* Last Logged: the flash log holds this for good, LASTLOG.DAT is a readable copy */
            flash_log_note_delivery(s_last_record_seq, true);
            const char *ts = slot_timestamp(telem);
            char last_log_ts[20] = "";
            if (strlen(ts) >= 12) {
                snprintf(last_log_ts, sizeof(last_log_ts), "%.2s-%.2s-%.4s %.2s:%.2s",
                         ts + 6, ts + 4, ts,
                         ts + 8, ts + 10);
            }
            if (last_log_ts[0] != '\0') {
                /* Fixed length: the file is overwritten in place */
                snprintf(s_slot_line, sizeof(s_slot_line), "%-19s\r\n", last_log_ts);
                flash_nvram_note_last_logged(last_log_ts);
                if (!storage_state_save(LAST_LOGGED_FILENAME, s_slot_line)) {
                    storage_log_debug("ERROR", "Could not save Last Logged time to SD card");
                }
            }

            /* Current slot succeeded: up to 15 backlog records, one try each */
            uint8_t backlogs_sent = 0;
            uint32_t bk_offset = 0;
            bool bk_failed = false;

            /* This slot is archived, so its payload buffer can carry the backlog records */
            while (backlogs_sent < 15 && SLOT_BUDGET_LEFT() && storage_backlog_get_next_unsent(0, &bk_offset, payload, 384)) {
                char bk_msg[64];
                snprintf(bk_msg, sizeof(bk_msg), "Sending Backlog #%d (offset %u)...", backlogs_sent + 1, (unsigned int)bk_offset);
                storage_log_debug("BACKLOG", bk_msg);
                hal_power_heartbeat();

                /* Carrier NAT needs this gap after a socket closes before opening the next */
                vTaskDelay(3000 / portTICK_RATE_MS);

                bool bk_ok = sim7600_http_post_once(endpoint, payload, good_route);

                if (bk_ok) {
                    storage_log_debug("BACKLOG", "Backlog delivered OK, marking SENT");
                    /* Unmarked, it would be picked and sent again straight away */
                    if (!storage_backlog_mark_sent(bk_offset)) {
                        storage_log_debug("ERROR", "Could not mark backlog record sent: stopping backlog cycle");
                        bk_failed = true;
                        break;
                    }
                    backlogs_sent++;
                } else if (sim7600_post_rejected()) {
                    /* This slot's own record was just accepted, so the server is working and it's this record */
                    if (storage_backlog_note_rejected(bk_offset, REJECTIONS_ALLOWED, payload)) {
                        storage_log_debug("ERROR", "Backlog record refused 3 times: moved to " REJECTED_FILENAME);
                    } else {
                        storage_log_debug("BACKLOG", "Backlog record refused: trying it again next slot");
                        bk_failed = true;
                        break;
                    }
                } else {
                    storage_log_debug("BACKLOG", "Backlog transmission failed! Aborting backlog cycle.");
                    bk_failed = true;
                    break;
                }
            }
            storage_backlog_cleanup_if_all_sent();

            /* A card that has failed took its queue with it: flash knows what those outages left */
            if (flash_log_outage_unsettled() && !storage_sd_is_ok()) {
                uint32_t adopted = flash_log_adopt_undelivered();
                for (uint32_t i = 0; i < adopted; i++) {
                    ui_inc_backlog_count();
                }
                snprintf(s_slot_line, sizeof(s_slot_line), "No SD card: %lu unsent records queued from flash", (unsigned long)adopted);
                storage_log_debug("BACKLOG", s_slot_line);
            }

            /* Then anything queued in flash while there was no SD card, within the same limit */
            uint32_t flash_seq = 0;
            while (!bk_failed && backlogs_sent < 15 && SLOT_BUDGET_LEFT() && flash_log_next_pending(&flash_seq, payload, 384)) {
                hal_power_heartbeat();
                vTaskDelay(3000 / portTICK_RATE_MS);

                if (sim7600_http_post_once(endpoint, payload, good_route)) {
                    storage_log_debug("BACKLOG", "Flash backlog record delivered OK");
                    flash_log_mark_sent(flash_seq);
                    ui_dec_backlog_count();
                    backlogs_sent++;
                } else if (sim7600_post_rejected()) {
                    if (flash_log_note_rejected(flash_seq, REJECTIONS_ALLOWED)) {
                        storage_log_debug("ERROR", "Flash backlog record refused 3 times: set aside, still in the flash log");
                        storage_append_stamped(REJECTED_FILENAME, payload);
                        ui_dec_backlog_count();
                    } else {
                        storage_log_debug("BACKLOG", "Flash backlog record refused: trying it again next slot");
                        bk_failed = true;
                    }
                } else {
                    storage_log_debug("BACKLOG", "Flash backlog transmission failed, aborting");
                    bk_failed = true;
                }
            }

            /* All delivered: a card lost from now on has nothing to take with it */
            if (!bk_failed && SLOT_BUDGET_LEFT() && flash_log_outage_unsettled() && storage_sd_is_ok() &&
                storage_backlog_get_pending_count() == 0) {
                flash_log_note_drained();
            }
        } else {
            storage_log_debug("HTTP", "Not delivered: saving slot as pending backlog.");
            *out_reason = sim7600_post_unanswered() ? "NO_REPLY" : fail_reason;
            result = save_slot_for_retry(telem, payload);
        }

        /* The record has gone out, so this can't delay it */
        if (SLOT_BUDGET_LEFT()) {
            gps_auto_step(telem);
        }

        /* Check and process incoming operator SMS */
        sim7600_check_incoming_sms(telem);
    } else {
        storage_log_debug("MODEM", "Modem has no network after all tries! Saving to backlog.");
        format_slot_payload(payload, sizeof(payload), telem, -113);
        archive_slot(telem, -113, payload);
        *out_reason = "NO_NETWORK";
        result = save_slot_for_retry(telem, payload);
    }

    sim7600_release();
    return result;
}

/* --- Missed Slots ---
 * The LPC1768 keeps the last completed slot on SD. If slots were missed, it asks the always-on
 * LPC845 for its stored samples and queues them as backlog records, so the server gets the
 * measured values. They carry the ESP32's gap-filled signal marker. */
#define SIGNAL_GAP_FILLED       (-104)
#define SIGNAL_OFF_FILLED_TODAY (-112)  /* ESP32 marker: not measured, this rain day */
#define SIGNAL_OFF_FILLED_PAST  (-110)  /* ESP32 marker: not measured, closing an earlier rain day */
#define GAP_MAX_SLOTS           (3 * 96)    /* The LPC845 keeps 3 rain days */
#define GAP_REPLY_WAIT_MS       3000

static void step_back_days(int *yr, int *mo, int *da, int days)
{
    static const int month_days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    while (days-- > 0) {
        if (--(*da) < 1) {
            if (--(*mo) < 1) {
                *mo = 12;
                (*yr)--;
            }
            *da = (*mo == 2 && (*yr % 4) == 0) ? 29 : month_days[*mo];
        }
    }
}

static void save_last_slot(int32_t slot_id)
{
    char line[16];
    snprintf(line, sizeof(line), "%010ld\r\n", (long)slot_id);
    if (!storage_state_save(LAST_SLOT_FILENAME, line)) {
        storage_log_debug("ERROR", "Could not save last slot to SD card");
    }
}

static int32_t load_last_slot(void)
{
    /* The newer of the two: with the card out, slots still reach flash but not this file */
    int32_t last = flash_log_newest_slot();
    char line[16];
    if (storage_state_load(LAST_SLOT_FILENAME, line, sizeof(line)) && strlen(line) >= 10) {
        int32_t on_card = atol(line);
        if (on_card > last) {
            last = on_card;
        }
    }
    return last;
}

static telemetry_data_t s_gap_record; /* Used by vMain_Task only */

static bool parse_record_total(const char *record, char *ts, size_t ts_size, unsigned long *out_tips);

/* GAP_RULE_BEGIN */
/* A slot nobody measured gets a row only if this unit holds a record from the same rain day, even a
 * 0 mm one. The start of a rain day with nothing known stays empty. Returns the row's signal marker,
 * or 0 for no row. */
static int off_slot_marker(const char *known_close, const char *gap_close, const char *current_close)
{
    if (known_close[0] == '\0' || strcmp(known_close, gap_close) != 0) {
        return 0;
    }
    return (strcmp(gap_close, current_close) == 0) ? SIGNAL_OFF_FILLED_TODAY : SIGNAL_OFF_FILLED_PAST;
}

/* The battery in a record, its last field, as 13.38. 0 if it isn't in that form. */
static float record_battery(const char *record)
{
    const char *q = strrchr(record, ',');
    if (!q) {
        return 0.0f;
    }
    q++;
    int whole = 0, digits = 0;
    while (*q >= '0' && *q <= '9') {
        whole = whole * 10 + (*q++ - '0');
        digits++;
    }
    int frac = 0, scale = 1;
    if (*q == '.') {
        q++;
        while (*q >= '0' && *q <= '9' && scale < 100) {
            frac = frac * 10 + (*q++ - '0');
            scale *= 10;
        }
    }
    return (digits > 0) ? (float)whole + (float)frac / (float)scale : 0.0f;
}
/* GAP_RULE_END */


static void __attribute__((noinline)) fill_missed_slots(const telemetry_data_t *telem, int32_t slot_id)
{
    int32_t last_done = load_last_slot();
    if (last_done < 0 || slot_id - last_done <= 1) {
        return; /* First run on this card, or nothing missed */
    }
    if (intermcu_get_845_system() < 0) {
        storage_log_debug("ERROR", "Slots were missed, but this LPC845 firmware can't supply stored samples");
        return;
    }

    int32_t missed = slot_id - last_done - 1;
    if (missed > GAP_MAX_SLOTS) {
        missed = GAP_MAX_SLOTS;
    }
    snprintf(s_slot_line, sizeof(s_slot_line), "%d slot(s) missed since last run, requesting from LPC845", (int)missed);
    storage_log_debug("GAP", s_slot_line);

    const char *p = telem->present_time;
    int base_yr = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
    int base_mo = (p[4]-'0')*10 + (p[5]-'0');
    int base_da = (p[6]-'0')*10 + (p[7]-'0');

    int filled = 0, off_filled = 0, not_held = 0;
    char request[16];
    char payload[512];

    /* What the newest record says about its rain day: CUM_RF carries on from it */
    char known_close[12] = "";
    char current_close[12] = "";
    float known_cum_mm = 0.0f;
    float known_bat = 0.0f;
    const flash_log_page_t *newest = flash_log_newest_record();
    unsigned long known_tips = 0;
    char known_ts[16];
#if (UNIT_CFG == KSNDMC_TWS)
    /* No rain total in a TWS record: only its time, "NN,YYYY-MM-DD,HH:MM,..." */
    if (newest && strlen(newest->record) >= 19 && newest->record[2] == ',' && newest->record[13] == ',') {
        const char *r = newest->record;
        snprintf(known_ts, sizeof(known_ts), "%.4s%.2s%.2s%.2s%.2s00", r + 3, r + 8, r + 11, r + 14, r + 17);
        if (intermcu_rain_close_date(known_ts, known_close, sizeof(known_close))) {
            known_bat = record_battery(r);
        }
    }
    (void)known_tips;
#else
    if (newest && parse_record_total(newest->record, known_ts, sizeof(known_ts), &known_tips) &&
        intermcu_rain_close_date(known_ts, known_close, sizeof(known_close))) {
        known_cum_mm = (float)known_tips * RF_BUCKET_RESOLUTION;
        known_bat = record_battery(newest->record);
    }
#endif
    intermcu_rain_close_date(telem->present_time, current_close, sizeof(current_close));

    for (int32_t k = missed; k >= 1; k--) {
        int32_t id = slot_id - k;
        int yr = base_yr, mo = base_mo, da = base_da;
        step_back_days(&yr, &mo, &da, (int)(slot_id / 96 - id / 96));
        int slot_min = (int)(id % 96) * 15;
        snprintf(request, sizeof(request), "%04d%02d%02d%02d%02d00", yr, mo, da, slot_min / 60, slot_min % 60);

        hal_power_heartbeat();
        intermcu_request_record(request);

        memcpy(&s_gap_record, telem, sizeof(s_gap_record));
        int state = 0;
        for (int waited = 0; waited < GAP_REPLY_WAIT_MS && state == 0; waited += 20) {
            vTaskDelay(20 / portTICK_RATE_MS);
            state = intermcu_poll_record(&s_gap_record);
        }

        if (state == 1) {
            format_slot_payload(payload, sizeof(payload), &s_gap_record, SIGNAL_GAP_FILLED);
            s_archive_slot_id = id;
            archive_slot(&s_gap_record, SIGNAL_GAP_FILLED, payload);
            slot_result_t result = save_slot_for_retry(&s_gap_record, payload);
            snprintf(s_slot_line, sizeof(s_slot_line), "%.14s,%.14s,%s,GAP_FILLED,0,%d,0,,,%u",
                     telem->present_time, request, (result == SLOT_LOST) ? "LOST" : "BACKLOG",
                     SIGNAL_GAP_FILLED, (unsigned int)ui_get_backlog_count());
            storage_log_debug("SLOT", s_slot_line);
            storage_log_slot(s_slot_line);
            filled++;
            /* Measured: the rows after it carry on from this */
            intermcu_rain_close_date(request, known_close, sizeof(known_close));
            known_cum_mm = s_gap_record.rf_cum_mm;
            known_bat = s_gap_record.bat1_volt;
        } else if (state == -1) {
            /* The LPC845 was off too, so this slot was never measured */
            char gap_close[12] = "";
            intermcu_rain_close_date(request, gap_close, sizeof(gap_close));
            int marker = off_slot_marker(known_close, gap_close, current_close);
#if (UNIT_CFG == KSNDMC_TWS)
            /* Temperature, humidity and wind weren't measured, and aren't estimated: the row stays empty */
            marker = 0;
#endif
            if (marker == 0) {
                not_held++;
                continue;
            }
            strncpy(s_gap_record.slot_time, request, sizeof(s_gap_record.slot_time) - 1);
            s_gap_record.slot_time[sizeof(s_gap_record.slot_time) - 1] = '\0';
            s_gap_record.rf_tips_15m = 0;
            s_gap_record.rf_15m_mm = 0.0f;
            s_gap_record.rf_cum_mm = known_cum_mm;
            s_gap_record.bat1_volt = known_bat; /* The signal value is what marks a filled row */
            format_slot_payload(payload, sizeof(payload), &s_gap_record, marker);
            s_archive_slot_id = id;
            archive_slot(&s_gap_record, marker, payload);
            slot_result_t result = save_slot_for_retry(&s_gap_record, payload);
            snprintf(s_slot_line, sizeof(s_slot_line), "%.14s,%.14s,%s,OFF_FILLED,0,%d,0,,,%u",
                     telem->present_time, request, (result == SLOT_LOST) ? "LOST" : "BACKLOG",
                     marker, (unsigned int)ui_get_backlog_count());
            storage_log_debug("SLOT", s_slot_line);
            storage_log_slot(s_slot_line);
            off_filled++;
        } else {
            storage_log_debug("ERROR", "LPC845 did not answer a stored-sample request, stopping gap fill");
            break;
        }
    }

    snprintf(s_slot_line, sizeof(s_slot_line), "Gap fill done: %d measured, %d not measured, %d left empty", filled, off_filled, not_held);
    storage_log_debug("GAP", s_slot_line);
    if (not_held > 0) {
        snprintf(s_slot_line, sizeof(s_slot_line), "%.14s,,EMPTY,NO_DATA_FOR_RAIN_DAY,0,0,0,,,%d", telem->present_time, not_held);
        storage_log_slot(s_slot_line);
    }
}

/* RAIN_PARSE_BEGIN */
/* Tips held in a TRG record ("NN,YYYY-MM-DD,HH:MM,rf15,cum,csq,bat"), with its time as
 * YYYYMMDDHHMM00. False if the record isn't in that form. */
static bool parse_record_total(const char *record, char *ts, size_t ts_size, unsigned long *out_tips)
{
    const char *p = record;
    const char *field[5];
    for (int i = 0; i < 5; i++) {
        field[i] = p;
        p = strchr(p, ',');
        if (!p) {
            return false;
        }
        p++;
    }
    /* field[1] is the date, [2] the time, [4] the total in mm, as 12.50 */
    if (field[2] - field[1] != 11 || field[3] - field[2] != 6) {
        return false;
    }
    snprintf(ts, ts_size, "%.4s%.2s%.2s%.2s%.2s00", field[1], field[1] + 5, field[1] + 8, field[2], field[2] + 3);

    unsigned long whole = 0, hundredths = 0;
    const char *q = field[4];
    while (*q >= '0' && *q <= '9') {
        whole = whole * 10 + (unsigned long)(*q++ - '0');
    }
    if (*q != '.' || q[1] < '0' || q[1] > '9' || q[2] < '0' || q[2] > '9') {
        return false;
    }
    hundredths = whole * 100 + (unsigned long)(q[1] - '0') * 10 + (unsigned long)(q[2] - '0');

    unsigned long bucket = (unsigned long)(RF_BUCKET_RESOLUTION * 100.0f + 0.5f);
    *out_tips = (hundredths + bucket / 2) / bucket;
    return true;
}
/* RAIN_PARSE_END */

/* CUM_RF must never dip. The LPC845 keeps the total in RAM, so after a restart it asks for it back:
 * this returns the total from the newest stored record, if that is from the same rain day. The
 * LPC845 adds it once, on top of any tips since the restart. Only the rain that fell while it had
 * no power is missing. */
static void rain_total_restore_step(const telemetry_data_t *telem)
{
    if (!intermcu_845_wants_rain_total()) {
        return;
    }

    static portTickType s_next_try = 0;
    portTickType now = xTaskGetTickCount();
    if ((long)(now - s_next_try) < 0) {
        return;
    }
    s_next_try = now + (3000 / portTICK_RATE_MS); /* Until a frame says the LPC845 has it */

    unsigned long tips = 0;
#if (UNIT_CFG != KSNDMC_TWS)
    const flash_log_page_t *rec = flash_log_newest_record();
    char ts[16], stored_close[12], current_close[12];
    if (rec && parse_record_total(rec->record, ts, sizeof(ts), &tips) &&
        intermcu_rain_close_date(ts, stored_close, sizeof(stored_close)) &&
        intermcu_rain_close_date(telem->present_time, current_close, sizeof(current_close)) &&
        strcmp(stored_close, current_close) == 0) {
        snprintf(s_slot_line, sizeof(s_slot_line), "LPC845 restarted: returning CUM_RF of %lu tips from record %.12s", tips, ts);
    } else {
        tips = 0;
        snprintf(s_slot_line, sizeof(s_slot_line), "LPC845 restarted: no stored total for this rain day");
    }
#else
    (void)telem;
    snprintf(s_slot_line, sizeof(s_slot_line), "LPC845 restarted: this profile has no rain total");
#endif
    storage_log_debug("RAIN", s_slot_line);
    hal_power_rain_restore(tips);
}

/* No clock means no record. A dead RTC with no network would otherwise hold the modem on for the
 * full wait at every slot. */
static bool __attribute__((noinline)) clock_wait_is_over(void)
{
    uint32_t up_sec = xTaskGetTickCount() / configTICK_RATE_HZ;
    if (up_sec > SLOT_CLOCK_WAIT_SEC) {
        return true;
    }
    modem_status_t status;
    sim7600_get_status(&status);
    return up_sec > SLOT_CLOCK_NO_NET_SEC && !status.is_registered;
}

/* Is the LPC845 sending good status frames right now? It sends one a second. */
#define LINK_CHECK_SEC 5
static bool lpc845_link_alive(void)
{
    uint32_t bytes, frames, good_before, good_after;
    intermcu_get_link_stats(&bytes, &frames, &good_before, NULL, 0);
    vTaskDelay((LINK_CHECK_SEC * 1000) / portTICK_RATE_MS);
    intermcu_get_link_stats(&bytes, &frames, &good_after, NULL, 0);
    return good_after != good_before;
}

#if (UNIT_CFG != KSNDMC_TWS)
/* CUM_RF never drops within a rain day, whatever went wrong elsewhere. Normally the LPC845's total
 * is the last record's plus this slot's rain, so this does nothing. */
static void __attribute__((noinline)) keep_cum_from_dipping(telemetry_data_t *telem)
{
    unsigned long tips = 0;
    char ts[16], stored_close[12], current_close[12];
    const flash_log_page_t *rec = flash_log_newest_record();
    if (!rec || !parse_record_total(rec->record, ts, sizeof(ts), &tips) ||
        !intermcu_rain_close_date(ts, stored_close, sizeof(stored_close)) ||
        !intermcu_rain_close_date(telem->present_time, current_close, sizeof(current_close)) ||
        strcmp(stored_close, current_close) != 0) {
        return; /* Nothing yet this rain day: a new day starts from zero */
    }
    float floor_mm = (float)tips * RF_BUCKET_RESOLUTION;
    /* This slot's rain counts on top, unless the newest record already is this slot */
    if (strncmp(ts, telem->slot_time, 12) != 0) {
        floor_mm += telem->rf_15m_mm;
    }
    if (telem->rf_cum_mm < floor_mm - 0.01f) {
        char have[8], keep[8];
        fmt_fixed(have, sizeof(have), telem->rf_cum_mm, 5, 2);
        fmt_fixed(keep, sizeof(keep), floor_mm, 5, 2);
        snprintf(s_slot_line, sizeof(s_slot_line), "CUM_RF would have dropped to %s mm, keeping %s mm", have, keep);
        storage_log_debug("ERROR", s_slot_line);
        telem->rf_cum_mm = floor_mm;
    }
}
#endif

/* Runs one slot from start to finish. The UI cannot power down while this is running. */
static void run_slot(telemetry_data_t *telem, int min, int sec, int32_t slot_id)
{
    ui_set_slot_busy(true);

    s_last_slot_id = slot_id;
    const portTickType slot_begin = xTaskGetTickCount();

    /* With the LPC845 silent (hung, or on a failed firmware trial) there's no rain, battery or
     * total to report, and a record of zeros would pull CUM_RF down. So nothing is sent or stored.
     * This slot stays unmarked, so the first good slot afterwards fills it in the usual way. */
    if (!lpc845_link_alive()) {
        snprintf(s_slot_line, sizeof(s_slot_line), "No status frames from LPC845: no record for slot %ld", (long)slot_id);
        storage_log_debug("ERROR", s_slot_line);
        ui_set_slot_busy(false);
        if (!ui_is_display_active() && !ui_is_operator_touched()) {
            ui_enter_sleep();
        }
        return;
    }

    /* Queue anything missed in the wait below, so it can't delay this slot's record */
    fill_missed_slots(telem, slot_id);
    s_archive_slot_id = slot_id;

    /* Slot boundary guard: servers reject a record sent under 62s after its slot boundary.
     * min and sec were read before the link check and gap fill, so that time counts too. */
    int elapsed_sec = (min % 15) * 60 + sec + (int)((xTaskGetTickCount() - slot_begin) / configTICK_RATE_HZ);
    if (elapsed_sec < SLOT_HOLD_GUARD_SEC) {
        int wait_sec = SLOT_HOLD_GUARD_SEC - elapsed_sec;
        snprintf(s_slot_line, sizeof(s_slot_line), "Slot boundary reached, holding %ds for sensors", wait_sec);
        storage_log_debug("SLOT", s_slot_line);
        for (int w = 0; w < wait_sec; w++) {
            vTaskDelay(1000 / portTICK_RATE_MS);
        }
    }
    /* Re-read fresh telemetry */
    intermcu_get_telemetry(telem);
#if (UNIT_CFG != KSNDMC_TWS)
    keep_cum_from_dipping(telem);
#endif

    const char *boot_mode = ui_was_scheduled_boot() ? "SCHEDULED" : "OPERATOR";
    char s_bat[8], s_rf15[8], s_rfday[8];
    fmt_fixed(s_rf15, sizeof(s_rf15), telem->rf_15m_mm, 5, 2);
    fmt_fixed(s_rfday, sizeof(s_rfday), telem->rf_cum_mm, 5, 2);
    fmt_fixed(s_bat, sizeof(s_bat), telem->bat1_volt, 4, 2);
    snprintf(s_slot_line, sizeof(s_slot_line), "time=%.14s slot=%.14s boot=%s bat=%sV rain15=%s rainday=%s",
             telem->present_time, telem->slot_time, boot_mode, s_bat, s_rf15, s_rfday);
    storage_log_debug("SLOT_START", s_slot_line);

    portTickType start_tick = xTaskGetTickCount();
    int attempts = 0;
    int csq = -113;
    const char *reason = "UNKNOWN";
    slot_result_t result = execute_slot_transmission(telem, &attempts, &csq, &reason);
    unsigned int duration_sec = (unsigned int)((xTaskGetTickCount() - start_tick) / configTICK_RATE_HZ);

    static const char *const result_names[] = {"SENT", "BACKLOG", "LOST"};
    snprintf(s_slot_line, sizeof(s_slot_line), "%.14s,%.14s,%s,%s,%d,%d,%u,%s,%s,%u",
             telem->present_time, telem->slot_time, result_names[result], reason, attempts, csq,
             duration_sec, s_bat, boot_mode, (unsigned int)ui_get_backlog_count());
    storage_log_debug("SLOT_END", s_slot_line);
    storage_log_slot(s_slot_line);
    {
        uint32_t link_bytes, link_frames, link_good;
        intermcu_get_link_stats(&link_bytes, &link_frames, &link_good, NULL, 0);
        snprintf(s_slot_line, sizeof(s_slot_line), "Status frames this boot: %lu good, %lu damaged",
                 (unsigned long)link_good, (unsigned long)intermcu_get_bad_frames());
        storage_log_debug("LINK", s_slot_line);
    }

    /* Only now: a restart mid-slot refills this one rather than losing it */
    save_last_slot(slot_id);

    ui_set_slot_busy(false);

    /* If station was woken in unattended sentry mode AND display is not active:
     * enter sleep. If operator is browsing or display is active, stay awake! */
    if (!ui_is_display_active() && !ui_is_operator_touched()) {
        storage_log_debug("PWR", "Unattended slot transmission complete -> entering sleep");
        ui_enter_sleep();
    } else {
        storage_log_debug("PWR", "Operator active on keypad/LCD, remaining awake");
    }
}

static void __attribute__((noinline)) log_boot_header(void)
{
    storage_log_mount();

    flash_log_init();
    uint32_t flash_pending = flash_log_pending_count();
    ui_set_backlog_count(ui_get_backlog_count() + flash_pending);

    char boot_info[140];
    snprintf(boot_info, sizeof(boot_info), "=== BOOT %s Station=%s Profile=%s ResetReason=0x%02X%s%s%s%s%s ===",
             FIRMWARE_VERSION, flash_nvram_get_station_id(),
             (UNIT_CFG == KSNDMC_TRG) ? "KSNDMC_TRG" :
             (UNIT_CFG == BIHAR_TRG) ? "BIHAR_TRG" : "KSNDMC_TWS",
             (unsigned int)s_reset_reason,
             (s_reset_reason & 0x01) ? " POWER-ON" : "",
             (s_reset_reason & 0x02) ? " EXTERNAL" : "",
             (s_reset_reason & 0x04) ? " WATCHDOG" : "",
             (s_reset_reason & 0x08) ? " BROWNOUT" : "",
             (s_reset_reason == 0) ? " SOFTWARE" : "");
    storage_log_debug("BOOT", boot_info);
    log_previous_crash();

    uint32_t bk_count = storage_backlog_get_pending_count();
    char bk_info[64];
    snprintf(bk_info, sizeof(bk_info), "Pending Backlogs: %u on SD, %u in flash", (unsigned int)bk_count, (unsigned int)flash_pending);
    storage_log_debug("BOOT", bk_info);

    snprintf(bk_info, sizeof(bk_info), "Flash log holds %u records", (unsigned int)flash_log_record_count());
    storage_log_debug("BOOT", bk_info);

    snprintf(bk_info, sizeof(bk_info), "Free heap after task creation: %u bytes", (unsigned int)xPortGetFreeHeapSize());
    storage_log_debug("BOOT", bk_info);

    /* Last Logged: the newest record the server got. The flash log needs no card; the SD copy is
     * only for a unit whose flash log has no such record. */
    const flash_log_page_t *delivered = flash_log_last_delivered();
    const char *r = delivered ? delivered->record : "";
    if (strlen(r) >= 19 && r[2] == ',' && r[13] == ',') {
        /* "NN,YYYY-MM-DD,HH:MM,..." to "DD-MM-YYYY HH:MM" */
        char last_logged[20];
        snprintf(last_logged, sizeof(last_logged), "%.2s-%.2s-%.4s %.5s", r + 11, r + 8, r + 3, r + 14);
        flash_nvram_note_last_logged(last_logged);
    } else {
        char last_logged[24];
        if (storage_state_load(LAST_LOGGED_FILENAME, last_logged, sizeof(last_logged))) {
            for (int i = (int)strlen(last_logged) - 1; i >= 0 && (last_logged[i] == ' ' || last_logged[i] == '\r' || last_logged[i] == '\n'); i--) {
                last_logged[i] = '\0';
            }
            flash_nvram_note_last_logged(last_logged);
        }
    }
}

static void vMain_Task(void *pvParameters)
{
    (void)pvParameters;

    log_boot_header();
    fw_app_init();

    uint32_t heartbeat_sec = 0;
    uint32_t link_log_sec = 0;

    for (;;) {
        /* LPC845 link health: soon after boot, then every 5 minutes */
        if (link_log_sec == 8 || (link_log_sec % 300) == 299) {
            uint32_t bytes, frames, good;
            char last[72];
            intermcu_get_link_stats(&bytes, &frames, &good, last, sizeof(last));
            char wake = '?';
            int reset_status = -1;
            intermcu_get_845_wake(&wake, &reset_status);
            snprintf(s_slot_line, sizeof(s_slot_line), "LPC845 link: %lu bytes, %lu frames, %lu good, %lu damaged, woke=%c 845reset=%d. Last: %s",
                     (unsigned long)bytes, (unsigned long)frames, (unsigned long)good, (unsigned long)intermcu_get_bad_frames(), wake, reset_status, last);
            storage_log_debug(good ? "LINK" : "ERROR", s_slot_line);
        }
        link_log_sec++;

        {
            uint32_t bytes, frames, good;
            intermcu_get_link_stats(&bytes, &frames, &good, NULL, 0);
            fw_app_step(xTaskGetTickCount() / configTICK_RATE_HZ, good);
        }

        /* Sent from this loop so it proves slot scheduling is alive, not just the UI */
        if (++heartbeat_sec >= 60) {
            heartbeat_sec = 0;
            hal_power_heartbeat();
        }

        telemetry_data_t telem;
        memset(&telem, 0, sizeof(telem));
        intermcu_get_telemetry(&telem);

        int min = 0, sec = 0;
        int32_t slot_id = 0;
        bool clock_ok = parse_slot_clock(&telem, &min, &sec, &slot_id);

        if (ui_is_boot_mode_known()) {
            if (clock_ok) {
                s_waiting_for_clock = false;

                rain_total_restore_step(&telem);

                if (!s_slot_armed) {
                    /* Due now if we booted inside a slot minute, or on a scheduled wake that
                     * came up late. The minute before a boundary just waits for the boundary. */
                    bool due_now = (min % 15) == 0 || (ui_was_scheduled_boot() && (min % 15) != 14);
                    s_last_slot_id = due_now ? (slot_id - 1) : slot_id;
                    s_slot_armed = true;
                    snprintf(s_slot_line, sizeof(s_slot_line), "Scheduler armed at %.14s: %s",
                             telem.present_time, due_now ? "slot due now" : "waiting for next slot");
                    storage_log_debug("SLOT", s_slot_line);
                }

                if (slot_id < s_last_slot_id) {
                    /* Clock was set backwards: don't treat the earlier time as a new slot */
                    storage_log_debug("SLOT", "Clock moved backwards, scheduler re-aligned");
                    s_last_slot_id = slot_id;
                } else if (slot_id > s_last_slot_id) {
                    run_slot(&telem, min, sec, slot_id);
                }
            } else if (ui_was_scheduled_boot() && !ui_is_display_active()) {
                /* No valid clock: let the modem supervisor try to get network time */
                if (!s_waiting_for_clock) {
                    s_waiting_for_clock = true;
                    storage_log_debug("ERROR", "Scheduled wake but clock is invalid: slot cannot run yet");
                }
                if (clock_wait_is_over()) {
                    storage_log_debug("ERROR", "Clock still invalid: SLOT LOST, entering sleep");
                    storage_log_slot("NO_CLOCK,NO_CLOCK,LOST,INVALID_CLOCK,0,-113,0,0.00,SCHEDULED,0");
                    ui_enter_sleep();
                }
            }
        }

        vTaskDelay(1000 / portTICK_RATE_MS);
    }
}

/* SIM7600 Modem Supervisor Task (Boots modem, monitors registration, polls CSQ/carrier/time) */
static void vModem_Task(void *pvParameters)
{
    (void)pvParameters;

    storage_log_debug("MODEM", "Modem Supervisor Task Started");

    for (;;) {
        if (ui_is_boot_mode_known() && (ui_is_display_active() || s_waiting_for_clock)) {
            sim7600_worker_step();
        }
        vTaskDelay(2000 / portTICK_RATE_MS);
    }
}

int main(void)
{
    /* Why did we reset? Logged at boot, then cleared so the next reset reads clean */
    s_reset_reason = LPC_SC->RSID;
    LPC_SC->RSID = 0x0F;

    /* Fast settling delay */
    for (volatile int d = 0; d < 50000; d++);

    hal_power_init();
    flash_nvram_init();
    ui_display_init();
    intermcu_protocol_init();
    storage_log_init();
    sim7600_init();

    /* Create FreeRTOS Tasks */
    /* UiTask at Priority 3 guarantees instant keypad scan and screen responsiveness */
    xTaskCreate(vUi_Task,     (const signed char *)"UiTask",     512, NULL, 3, NULL);
    xTaskCreate(vModem_Task,  (const signed char *)"ModemTask",  768, NULL, 2, NULL);
    xTaskCreate(vMain_Task,   (const signed char *)"MainTask",   960, NULL, 1, NULL);
    xTaskCreate(vDisk_Task,   (const signed char *)"DiskTask",   512, NULL, 1, NULL);

    /* Start the scheduler */
    vTaskStartScheduler();

    for (;;);
    return 0;
}

