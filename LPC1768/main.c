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

    for (;;) {
        telemetry_data_t telem;
        intermcu_get_telemetry(&telem);

        modem_status_t modem_stat;
        sim7600_get_status(&modem_stat);

        ui_display_task_step(&telem, modem_stat.csq_dbm, modem_stat.ip_addr);

        vTaskDelayUntil(&xLastWakeTime, 50 / portTICK_RATE_MS);
    }
}

static void execute_slot_transmission(telemetry_data_t *telem)
{
    storage_log_debug("MODEM", "Booting SIM7600 4G LTE Modem...");
    if (sim7600_power_on()) {
        modem_status_t status;
        sim7600_get_status(&status);

        /* Format HTTP payload based on active configuration */
        char payload[512] = "";
        const http_endpoint_t *endpoint = telemetry_get_endpoint(ACTIVE_CONFIG);
        const char *stn_id = flash_nvram_get_station_id();

        #if (ACTIVE_CONFIG == CFG_KSNDMC_TRG)
        format_payload_ksndmc_trg(payload, sizeof(payload), stn_id, telem, status.csq_dbm);
        #elif (ACTIVE_CONFIG == CFG_BIHAR_TRG)
        format_payload_bihar_trg(payload, sizeof(payload), stn_id, telem, status.csq_dbm);
        #else
        format_payload_ksndmc_tws(payload, sizeof(payload), stn_id, telem, status.csq_dbm);
        #endif

        char log_buf[256];
        snprintf(log_buf, sizeof(log_buf), "Endpoint: %s:%d%s", endpoint->ip, endpoint->port, endpoint->path);
        storage_log_debug("HTTP", log_buf);
        storage_log_debug("HTTP_POST", payload);

        /* Transmit payload via direct Layer-4 TCP socket */
        char date_hdr[64] = "";
        bool post_ok = sim7600_http_post(endpoint, payload, date_hdr, sizeof(date_hdr));

        if (post_ok) {
            storage_log_debug("HTTP", "Transmission succeeded (200 OK)!");
            /* Archive to SD card /DATA/<YYMMDD>.TXT */
            char date6[8] = "";
            if (strlen(telem->slot_time) >= 8) {
                snprintf(date6, sizeof(date6), "%.6s", telem->slot_time + 2);
                storage_archive_slot(date6, payload);
                storage_log_debug("SDC", "Archived slot record to SD card");
            }

            /* Persist Last Logged date/time in Sector 1 Flash NVRAM (survives power off) */
            char last_log_ts[20] = "";
            if (strlen(telem->slot_time) >= 12) {
                snprintf(last_log_ts, sizeof(last_log_ts), "%.2s-%.2s-%.4s %.2s:%.2s",
                         telem->slot_time + 6, telem->slot_time + 4, telem->slot_time,
                         telem->slot_time + 8, telem->slot_time + 10);
            } else if (strlen(telem->present_time) >= 12) {
                snprintf(last_log_ts, sizeof(last_log_ts), "%.2s-%.2s-%.4s %.2s:%.2s",
                         telem->present_time + 6, telem->present_time + 4, telem->present_time,
                         telem->present_time + 8, telem->present_time + 10);
            }
            if (last_log_ts[0] != '\0') {
                flash_nvram_set_last_logged(last_log_ts);
                storage_log_debug("NVRAM", "Saved Last Logged timestamp to Sector 1 Flash");
            }
        } else {
            storage_log_debug("HTTP", "Transmission failed, falling back to SD storage");
        }

        /* Check and process incoming operator SMS */
        sim7600_check_incoming_sms(telem);

        /* Deterministic modem power off */
        sim7600_power_off();
    } else {
        storage_log_debug("MODEM", "Modem power-on or network registration failed!");
    }
}

static void vMain_Task(void *pvParameters)
{
    (void)pvParameters;

    storage_log_debug("BOOT", "AIO Unified Telemetry Firmware v6.1 Started");

    int last_transmitted_min = -1;

    for (;;) {
        telemetry_data_t telem;
        bool telem_ready = intermcu_get_telemetry(&telem);

        if (telem_ready && strlen(telem.present_time) >= 14) {
            int min = (telem.present_time[10] - '0') * 10 + (telem.present_time[11] - '0');
            int sec = (telem.present_time[12] - '0') * 10 + (telem.present_time[13] - '0');

            bool is_slot_boundary = (min % 15 == 0);

            if (is_slot_boundary && (last_transmitted_min != min)) {
                /* Slot boundary guard: ensure 62s have passed so sensors are settled */
                if (sec < SLOT_HOLD_GUARD_SEC) {
                    int wait_sec = SLOT_HOLD_GUARD_SEC - sec;
                    char sched_msg[64];
                    snprintf(sched_msg, sizeof(sched_msg), "Slot boundary %02d:00 reached, holding %ds", min, wait_sec);
                    storage_log_debug("SCHED", sched_msg);
                    vTaskDelay((wait_sec * 1000) / portTICK_RATE_MS);
                    /* Re-read fresh telemetry */
                    intermcu_get_telemetry(&telem);
                }

                last_transmitted_min = min;

                char trigger_msg[64];
                snprintf(trigger_msg, sizeof(trigger_msg), "15-Minute Trigger fired for slot %02d (Time: %.14s)", min, telem.present_time);
                storage_log_debug("SCHED", trigger_msg);

                execute_slot_transmission(&telem);

                /* If operator is not actively interacting with LCD/keypad, sleep system */
                if (!ui_is_operator_active()) {
                    storage_log_debug("PWR", "Slot transmission complete and no operator active -> sleeping ($f1;)");
                    vTaskDelay(200 / portTICK_RATE_MS);
                    hal_power_sleep_system();
                } else {
                    storage_log_debug("PWR", "Operator active on keypad/LCD, remaining awake");
                }
            }
        }

        vTaskDelay(1000 / portTICK_RATE_MS);
    }
}

/* UART3 Inter-MCU Receiver Task */
static void vUart3_Task(void *pvParameters)
{
    (void)pvParameters;

    for (;;) {
        /* Hardware ISR UART3_IRQHandler handles all incoming bytes from LPC845 in real-time.
         * Periodic sleep keeps task context alive. */
        vTaskDelay(1000 / portTICK_RATE_MS);
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
    xTaskCreate(vUart3_Task,  (const signed char *)"Uart3Task",  256, NULL, 2, NULL);
    xTaskCreate(vMain_Task,   (const signed char *)"MainTask",   768, NULL, 1, NULL);
    xTaskCreate(vDisk_Task,   (const signed char *)"DiskTask",   512, NULL, 1, NULL);

    /* Start the scheduler */
    vTaskStartScheduler();

    for (;;);
    return 0;
}

