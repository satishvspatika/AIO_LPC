#include "FreeRTOS.h"
#include "task.h"
#include "hal_power.h"
#include "intermcu_protocol.h"
#include "telemetry_formatter.h"
#include "sim7600_driver.h"
#include "ui_display.h"
#include "storage_log.h"
#include "flash_nvram.h"
#include "user_config.h"
#include "lpc17xx_uart.h"
#include <stdio.h>
#include <string.h>

static void vDisk_Task(void *pvParameters)
{
    (void)pvParameters;
    portTickType xLastWakeTime = xTaskGetTickCount();

    for (;;) {
        storage_log_timer_step_10ms();
        storage_log_drain_step();
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
        intermcu_get_telemetry(&telem);

        modem_status_t modem_stat;
        sim7600_get_status(&modem_stat);

        ui_display_task_step(&telem, &modem_stat);

        vTaskDelayUntil(&xLastWakeTime, 50 / portTICK_RATE_MS);
    }
}

static void execute_slot_transmission(telemetry_data_t *telem)
{
    if (!sim7600_acquire(5000)) {
        storage_log_debug("MODEM", "Modem busy, deferring slot transmission");
        return;
    }

    storage_log_debug("MODEM", "SIM7600 slot transmission starting...");
    if (sim7600_power_on()) {
        modem_status_t status;
        sim7600_get_status(&status);

        /* Format HTTP payload based on active configuration */
        char payload[512] = "";
        const http_endpoint_t *endpoint = telemetry_get_endpoint(UNIT_CFG);
        const char *stn_id = flash_nvram_get_station_id();

        #if (UNIT_CFG == KSNDMC_TRG)
        format_payload_ksndmc_trg(payload, sizeof(payload), stn_id, telem, status.csq_dbm);
        #elif (UNIT_CFG == BIHAR_TRG)
        format_payload_bihar_trg(payload, sizeof(payload), stn_id, telem, status.csq_dbm);
        #else
        format_payload_ksndmc_tws(payload, sizeof(payload), stn_id, telem, status.csq_dbm);
        #endif

        char log_buf[256];
        snprintf(log_buf, sizeof(log_buf), "Endpoint: %s:%d%s", endpoint->ip, endpoint->port, endpoint->path);
        storage_log_debug("HTTP", log_buf);
        storage_log_debug("HTTP_POST", payload);

        /* Transmit current slot with up to 2 retries (3 attempts total) */
        char date_hdr[64] = "";
        bool post_ok = false;
        for (int attempt = 1; attempt <= 3; attempt++) {
            char att_msg[48];
            snprintf(att_msg, sizeof(att_msg), "HTTP Post Attempt %d/3...", attempt);
            storage_log_debug("HTTP", att_msg);

            post_ok = sim7600_http_post(endpoint, payload, date_hdr, sizeof(date_hdr));
            if (post_ok) {
                storage_log_debug("HTTP", "Transmission succeeded (200 OK)!");
                break;
            }
            if (attempt < 3) {
                storage_log_debug("HTTP", "Attempt failed, waiting 3s before retry...");
                vTaskDelay(3000 / portTICK_RATE_MS);
            }
        }

        if (post_ok) {
            /* Archive to SD card /DATA/<YYMMDD>.TXT */
            const char *ts = (strlen(telem->slot_time) >= 8 && strncmp(telem->slot_time, "20", 2) == 0 && telem->slot_time[2] != '0')
                             ? telem->slot_time : telem->present_time;
            char date6[8] = "";
            if (strlen(ts) >= 8) {
                snprintf(date6, sizeof(date6), "%.6s", ts + 2);
                storage_archive_slot(date6, payload);
                storage_log_debug("SDC", "Archived slot record to SD card");
            }

            /* Persist Last Logged date/time in Sector 1 Flash NVRAM (survives power off) */
            char last_log_ts[20] = "";
            if (strlen(ts) >= 12) {
                snprintf(last_log_ts, sizeof(last_log_ts), "%.2s-%.2s-%.4s %.2s:%.2s",
                         ts + 6, ts + 4, ts,
                         ts + 8, ts + 10);
            }
            if (last_log_ts[0] != '\0') {
                flash_nvram_set_last_logged(last_log_ts);
                storage_log_debug("NVRAM", "Saved Last Logged timestamp to Sector 1 Flash");
            }

            /* Current slot succeeded: Check and process pending backlogs (max 15, no retries) */
            uint8_t backlogs_sent = 0;
            uint32_t bk_offset = 0;
            char bk_payload[384];

            while (backlogs_sent < 15 && storage_backlog_get_next_unsent(0, &bk_offset, bk_payload, sizeof(bk_payload))) {
                char bk_msg[64];
                snprintf(bk_msg, sizeof(bk_msg), "Sending Backlog #%d (offset %u)...", backlogs_sent + 1, (unsigned int)bk_offset);
                storage_log_debug("BACKLOG", bk_msg);

                char bk_date_hdr[64] = "";
                bool bk_ok = sim7600_http_post(endpoint, bk_payload, bk_date_hdr, sizeof(bk_date_hdr));

                if (bk_ok) {
                    storage_log_debug("BACKLOG", "Backlog delivered OK, marking SENT");
                    storage_backlog_mark_sent(bk_offset);
                    backlogs_sent++;
                    vTaskDelay(1500 / portTICK_RATE_MS);
                } else {
                    storage_log_debug("BACKLOG", "Backlog transmission failed! Aborting backlog cycle.");
                    break;
                }
            }
            storage_backlog_cleanup_if_all_sent();

        } else {
            storage_log_debug("HTTP", "All 3 HTTP attempts failed! Saving slot as pending backlog.");
            storage_backlog_save(telem->slot_time, payload);

            const char *ts = (strlen(telem->slot_time) >= 8 && strncmp(telem->slot_time, "20", 2) == 0 && telem->slot_time[2] != '0')
                             ? telem->slot_time : telem->present_time;
            char date6[8] = "";
            if (strlen(ts) >= 8) {
                snprintf(date6, sizeof(date6), "%.6s", ts + 2);
                storage_archive_slot(date6, payload);
            }
        }

        /* Check and process incoming operator SMS */
        sim7600_check_incoming_sms(telem);
    } else {
        storage_log_debug("MODEM", "Modem power-on or network registration failed! Saving to backlog.");
        char payload[512] = "";
        const char *stn_id = flash_nvram_get_station_id();
        #if (UNIT_CFG == KSNDMC_TRG)
        format_payload_ksndmc_trg(payload, sizeof(payload), stn_id, telem, -113);
        #elif (UNIT_CFG == BIHAR_TRG)
        format_payload_bihar_trg(payload, sizeof(payload), stn_id, telem, -113);
        #else
        format_payload_ksndmc_tws(payload, sizeof(payload), stn_id, telem, -113);
        #endif
        storage_backlog_save(telem->slot_time, payload);

        const char *ts = (strlen(telem->slot_time) >= 8 && strncmp(telem->slot_time, "20", 2) == 0 && telem->slot_time[2] != '0')
                         ? telem->slot_time : telem->present_time;
        char date6[8] = "";
        if (strlen(ts) >= 8) {
            snprintf(date6, sizeof(date6), "%.6s", ts + 2);
            storage_archive_slot(date6, payload);
        }
    }

    sim7600_release();
}

static void vMain_Task(void *pvParameters)
{
    (void)pvParameters;

    char boot_info[128];
    snprintf(boot_info, sizeof(boot_info), "AIO Telemetry Station %s Started (Station ID: %s, Profile: %s)",
             FIRMWARE_VERSION, flash_nvram_get_station_id(),
             (UNIT_CFG == KSNDMC_TRG) ? "KSNDMC_TRG" :
             (UNIT_CFG == BIHAR_TRG) ? "BIHAR_TRG" : "KSNDMC_TWS");
    storage_log_debug("BOOT", boot_info);

    uint32_t bk_count = storage_backlog_get_pending_count();
    char bk_info[64];
    snprintf(bk_info, sizeof(bk_info), "Pending Backlogs in Storage: %u", (unsigned int)bk_count);
    storage_log_debug("BOOT", bk_info);

    int last_transmitted_min = -1;
    bool boot_initialized = false;

    for (;;) {
        telemetry_data_t telem;
        bool telem_ready = intermcu_get_telemetry(&telem);

        if (telem_ready && strlen(telem.present_time) >= 14 && strncmp(telem.present_time, "20", 2) == 0) {
            int yr = (telem.present_time[0]-'0')*1000 + (telem.present_time[1]-'0')*100 + (telem.present_time[2]-'0')*10 + (telem.present_time[3]-'0');
            int min = (telem.present_time[10] - '0') * 10 + (telem.present_time[11] - '0');
            int sec = (telem.present_time[12] - '0') * 10 + (telem.present_time[13] - '0');

            if (yr >= 2024) {
                if (!boot_initialized) {
                    boot_initialized = true;
                    if (ui_is_display_active() || telem.usb_power_flag == 'U') {
                        last_transmitted_min = min;
                        storage_log_debug("SCHED", "Interactive boot detected: disarmed immediate boot slot transmission");
                    }
                }

                bool is_slot_boundary = (min % 15 == 0);

                if (is_slot_boundary && (last_transmitted_min != min)) {
                    /* Slot boundary guard: ensure 62s have passed so sensors are settled */
                    if (sec < SLOT_HOLD_GUARD_SEC) {
                        int wait_sec = SLOT_HOLD_GUARD_SEC - sec;
                        char sched_msg[64];
                        snprintf(sched_msg, sizeof(sched_msg), "Slot boundary %02d:00 reached, holding %ds", min, wait_sec);
                        storage_log_debug("SCHED", sched_msg);
                        for (int w = 0; w < wait_sec; w++) {
                            vTaskDelay(1000 / portTICK_RATE_MS);
                        }
                        /* Re-read fresh telemetry */
                        intermcu_get_telemetry(&telem);
                    }

                    last_transmitted_min = min;

                    char trigger_msg[64];
                    snprintf(trigger_msg, sizeof(trigger_msg), "15-Minute Trigger fired for slot %02d (Time: %.14s)", min, telem.present_time);
                    storage_log_debug("SCHED", trigger_msg);

                    execute_slot_transmission(&telem);

                    /* If station was woken in unattended sentry mode AND display is not active:
                     * enter sleep. If operator is browsing or display is active, stay awake! */
                    if (!ui_is_display_active() && !ui_is_operator_touched()) {
                        storage_log_debug("PWR", "Unattended slot transmission complete -> entering sleep");
                        ui_enter_sleep();
                    } else {
                        storage_log_debug("PWR", "Operator active on keypad/LCD, remaining awake");
                    }
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
        if (ui_is_display_active()) {
            sim7600_worker_step();
        }
        vTaskDelay(2000 / portTICK_RATE_MS);
    }
}

int main(void)
{
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
    xTaskCreate(vMain_Task,   (const signed char *)"MainTask",   768, NULL, 1, NULL);
    xTaskCreate(vDisk_Task,   (const signed char *)"DiskTask",   512, NULL, 1, NULL);

    /* Start the scheduler */
    vTaskStartScheduler();

    for (;;);
    return 0;
}

