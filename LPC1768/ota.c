/* Firmware over the air.
 *
 * Only brings the firmware file to the SD card. The file is the same .hex that goes on a card by hand,
 * and it is then checked and installed by the same code as MENU > FIRMWARE UPDATE (fw_update.c, the
 * bootloader's swap, trial and rollback). Nothing here writes to flash.
 *
 * Once a day, after the slot's record has gone, the unit asks the server whether it should run another
 * image. If so it fetches the file in pieces of OTA_PIECE bytes, each with its own CRC, straight onto
 * the card: the file's size on the card is how far the download has got, so a broken download goes on
 * from there at the next slot. The server learns the result from the next check (the running image's
 * CRC, and what the unit says about the one it tried).
 *
 * OTA.DAT on the card: "<file crc> <image crc> <file bytes> <phase>", phase being
 *   D  downloading that file            I  install started for that image
 *   F  that image did not stay (rolled back), or its file was refused: not taken again
 */
#include "ota.h"
#include "user_config.h"
#include "sim7600_driver.h"
#include "storage_log.h"
#include "flash_nvram.h"
#include "fw_update.h"
#include "fw_app.h"
#include "hal_power.h"
#include "intermcu_protocol.h"
#include "LPC17xx.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OTA_PIECE               1024U   /* The reply (headers + this) must fit the driver's 2 KB buffer */
#define OTA_MAX_FILE            600000UL
#define OTA_STOP_BEFORE_SLOT_S  330     /* No new piece is asked for this close to the next slot */
#define OTA_INSTALL_NEEDS_S     300     /* Check, copy to staging and restart, clear of the next slot */

typedef struct {
    uint32_t file_crc;
    uint32_t image_crc;
    uint32_t size;
    char phase;
} ota_state_t;

/* Static: this runs on the main task's stack, under the slot */
static char s_text[112];
static char s_path[96];
static char s_host[44];
static uint16_t s_port;
static ota_state_t s_st;
static fw_file_info_t s_info;
static portTickType s_slot_boundary;    /* Tick of the next slot boundary */
static bool s_install_ready = false;    /* The whole file is on the card */

/* CRC-32 as zlib's, a bit at a time (no table: a piece is 1 KB) */
static uint32_t ota_crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFUL;
    while (len--) {
        crc ^= *data++;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
        }
    }
    return ~crc;
}

static void ota_log(const char *tag, const char *msg)
{
    storage_log_debug(tag, msg);
}

static void ota_save(void)
{
    /* Fixed length: the file is overwritten in place */
    snprintf(s_text, sizeof(s_text), "%08lX %08lX %07lu %c\r\n", (unsigned long)s_st.file_crc,
             (unsigned long)s_st.image_crc, (unsigned long)s_st.size, s_st.phase);
    if (!storage_state_save(OTA_STATE_FILENAME, s_text)) {
        ota_log("ERROR", "OTA: could not write OTA.DAT");
    }
}

static void ota_load(void)
{
    unsigned long file_crc = 0, image_crc = 0, size = 0;
    char phase = '-';
    memset(&s_st, 0, sizeof(s_st));
    s_st.phase = '-';
    if (storage_state_load(OTA_STATE_FILENAME, s_text, sizeof(s_text)) &&
        sscanf(s_text, "%8lx %8lx %lu %c", &file_crc, &image_crc, &size, &phase) == 4 &&
        (phase == 'D' || phase == 'I' || phase == 'F')) {
        s_st.file_crc = file_crc;
        s_st.image_crc = image_crc;
        s_st.size = size;
        s_st.phase = phase;
    }
}

static void ota_forget_file(char phase)
{
    storage_state_delete(OTA_FILE_FILENAME);
    s_st.phase = phase;
    ota_save();
}

static long seconds_to_slot(void)
{
    return (long)(s_slot_boundary - xTaskGetTickCount()) / (long)configTICK_RATE_HZ;
}

static void ota_progress(const char *stage, unsigned percent)
{
    (void)stage;
    (void)percent;
    hal_power_heartbeat();
    wd_checkin();
}

/* The server's address: this build's, unless the card says otherwise ("host port") */
static void ota_server(void)
{
    unsigned port = OTA_SERVER_PORT;
    snprintf(s_host, sizeof(s_host), "%s", OTA_SERVER_HOST);
    if (storage_state_load(OTA_SERVER_FILENAME, s_text, sizeof(s_text))) {
        char host[sizeof(s_host)] = "";
        if (sscanf(s_text, "%43s %u", host, &port) >= 1 && strchr(host, '.') && port > 0 && port < 65536) {
            snprintf(s_host, sizeof(s_host), "%s", host);
        } else {
            port = OTA_SERVER_PORT;
        }
    }
    s_port = (uint16_t)port;
}

/* The file is whole: check it as a card file is checked, then hand it to the bootloader */
static void ota_install(void)
{
    if (seconds_to_slot() < OTA_INSTALL_NEEDS_S) {
        ota_log("OTA", "File complete: installing after the next slot (too close to it now)");
        return;
    }
    fw_result_t r = fw_update_check(OTA_FILE_FILENAME, UNIT_CFG, &s_info, ota_progress);
    if (r == FW_OK && s_info.crc32 != s_st.image_crc) {
        r = FW_E_BAD_CRC;
    }
    if (r == FW_OK) {
        r = fw_update_stage(OTA_FILE_FILENAME, &s_info, ota_progress);
        if (r == FW_OK) {
            /* Marked only now: a power cut during the copy leaves the download to be tried again */
            s_st.phase = 'I';
            ota_save();
            snprintf(s_text, sizeof(s_text), "OTA: %.16s (%08lX) staged and checked, restarting to install",
                     s_info.version, (unsigned long)s_info.crc32);
            ota_log("FW", s_text);
            storage_log_flush();
            vTaskDelay(1500 / portTICK_RATE_MS);
            NVIC_SystemReset();
        }
    }
    snprintf(s_text, sizeof(s_text), "OTA: file not installed: %s. Not taken again", fw_update_error_text(r));
    ota_log("ERROR", s_text);
    ota_forget_file('F');
}

static void ota_download(const char *stn_id)
{
    long have = storage_file_size(OTA_FILE_FILENAME);
    if (have < 0 || (uint32_t)have > s_st.size) {
        have = 0;
    }
    if ((uint32_t)have < s_st.size) {
        if (!storage_ota_open(OTA_FILE_FILENAME, (uint32_t)have)) {
            ota_log("ERROR", "OTA: could not open the download file on the card");
            return;
        }
        snprintf(s_text, sizeof(s_text), "OTA: downloading %lu bytes, from %ld", (unsigned long)s_st.size, have);
        ota_log("OTA", s_text);
        int bad = 0;
        unsigned next_note = 0;
        while ((uint32_t)have < s_st.size && bad < 3) {
            if (seconds_to_slot() < OTA_STOP_BEFORE_SLOT_S) {
                break;
            }
            hal_power_heartbeat();
            wd_checkin();
            const uint32_t n = (s_st.size - (uint32_t)have > OTA_PIECE) ? OTA_PIECE : (s_st.size - (uint32_t)have);
            int len = 0, status = 0;
            snprintf(s_path, sizeof(s_path), "/lpc/fw?id=%.12s&c=%08lX&o=%ld&n=%lu", stn_id,
                     (unsigned long)s_st.file_crc, have, (unsigned long)n);
            const char *body = sim7600_http_get(s_host, s_port, s_path, &len, &status);
            if (body && status == 404) {
                /* The office took that file away: start again from the next check */
                storage_ota_close();
                ota_log("OTA", "OTA: the server no longer has that file: download dropped");
                ota_forget_file('-');
                return;
            }
            if (!body || status != 200 || len != (int)n + 10 || body[0] != '@' || body[9] != '\n' ||
                strtoul(body + 1, NULL, 16) != ota_crc32((const uint8_t *)body + 10, n)) {
                bad++;
                continue;
            }
            if (!storage_ota_write(body + 10, n)) {
                ota_log("ERROR", "OTA: card write failed");
                bad = 3;
                break;
            }
            bad = 0;
            have += (long)n;
            const unsigned percent = (unsigned)((uint32_t)have / (s_st.size / 100U + 1U));
            if (percent >= next_note) {
                next_note = percent + 20U;
                snprintf(s_text, sizeof(s_text), "OTA: %ld of %lu bytes", have, (unsigned long)s_st.size);
                ota_log("OTA", s_text);
            }
        }
        storage_ota_close();
        if ((uint32_t)have < s_st.size) {
            snprintf(s_text, sizeof(s_text), "OTA: stopped at %ld of %lu bytes (%s): goes on at the next slot", have,
                     (unsigned long)s_st.size, (bad >= 3) ? "no good reply" : "next slot is near");
            ota_log("OTA", s_text);
            return;
        }
    }
    s_install_ready = true;     /* Done by ota_install_step, once the slot has been recorded */
}

void ota_install_step(void)
{
    if (s_install_ready) {
        s_install_ready = false;
        ota_install();
    }
}

void ota_slot_step(int slot)
{
    /* Taken now: the slot's own copy is from its start, and this needs the time left to the next slot */
    static telemetry_data_t s_now;
    const telemetry_data_t *telem = &s_now;
    if (!intermcu_get_telemetry(&s_now)) {
        return;
    }
    const char *stn_id = flash_nvram_get_station_id();
    const char *t = telem->present_time;
    const uint32_t running = fw_app_running_crc();
    bool due = (slot == OTA_DAILY_FIRST_SLOT + (int)(strtoul(stn_id, NULL, 10) % OTA_DAILY_SPREAD_SLOTS));

    if (!storage_sd_is_ok() || fw_app_trial_unconfirmed() || storage_backlog_get_pending_count() != 0 || strlen(t) < 14) {
        return;
    }
    ota_load();
    if (s_st.phase == 'I') {
        /* An install was started: did the new image stay? */
        if (s_st.image_crc == running) {
            ota_log("FW", "OTA: the new firmware is running");
            ota_forget_file('-');
        } else {
            ota_log("ERROR", "OTA: the new firmware did not stay (went back to the old one). Not taken again");
            ota_forget_file('F');
        }
        due = true;     /* Tell the server now */
    }
    if (!due && s_st.phase != 'D' && storage_file_size(OTA_NOW_FILENAME) < 0) {
        return;
    }
    if (telem->bat1_volt >= BATTERY_SENSE_FAULT_V && telem->bat1_volt < GPS_MIN_BATTERY_V) {
        ota_log("OTA", "OTA: battery low, not checking for firmware");
        return;
    }
    s_slot_boundary = xTaskGetTickCount() + (portTickType)((15 - (((t[10] - '0') * 10 + (t[11] - '0')) % 15)) * 60 -
                                                           ((t[12] - '0') * 10 + (t[13] - '0'))) * configTICK_RATE_HZ;

    ota_server();
    int len = 0, status = 0;
    unsigned long size = 0, file_crc = 0, image_crc = 0;
    snprintf(s_path, sizeof(s_path), "/lpc/check?id=%.12s&p=%d&v=%s&c=%08lX&s=%c&t=%08lX", stn_id, (int)UNIT_CFG,
             FIRMWARE_VERSION, (unsigned long)running, (s_st.phase == 'D') ? '1' : (s_st.phase == 'F') ? '3' : '0',
             (unsigned long)((s_st.phase == 'F') ? s_st.image_crc : 0UL));
    hal_power_heartbeat();
    const char *body = sim7600_http_get(s_host, s_port, s_path, &len, &status);
    const char *answer = (body && status == 200) ? strstr(body, "OTA ") : NULL;
    if (!answer) {
        snprintf(s_text, sizeof(s_text), "OTA: no answer from %.40s (HTTP %d)", s_host, status);
        ota_log("OTA", s_text);
    } else if (sscanf(answer, "OTA FW %lu %8lx %8lx", &size, &file_crc, &image_crc) != 3) {
        ota_log("OTA", "OTA: nothing new");
        if (s_st.phase == 'D') {
            ota_forget_file('-');
        }
    } else if (size < 1000UL || size > OTA_MAX_FILE || image_crc == running) {
        ota_log("OTA", "OTA: offer ignored (size, or the image already running)");
    } else if (s_st.phase == 'F' && s_st.image_crc == image_crc) {
        ota_log("OTA", "OTA: the offered firmware failed here before: not taken");
    } else {
        if (s_st.phase != 'D' || s_st.file_crc != file_crc || s_st.size != size) {
            storage_state_delete(OTA_FILE_FILENAME);
            s_st.file_crc = file_crc;
            s_st.image_crc = image_crc;
            s_st.size = size;
            s_st.phase = 'D';
            ota_save();
        }
        ota_download(stn_id);
    }
    sim7600_http_get_close();
}
