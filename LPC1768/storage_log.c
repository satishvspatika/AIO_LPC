#include "storage_log.h"
#include "user_config.h"
#include "ui_display.h"
#include "intermcu_protocol.h"
#include "ff.h"
#include "diskio.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <stdio.h>
#include <string.h>

#define RING_BUFFER_SIZE 2048

static FATFS s_fatfs;
static bool s_fs_mounted = false;
static bool s_debug_dump_enabled = SDC_DEBUG_DUMP_DEFAULT;

static char s_ring_buffer[RING_BUFFER_SIZE];
static volatile uint16_t s_head = 0;
static volatile uint16_t s_tail = 0;

/* FreeRTOS Mutex for thread-safe FatFs access across tasks */
static xSemaphoreHandle s_fs_mutex = NULL;

static void fs_lock(void)
{
    if (!s_fs_mutex) {
        s_fs_mutex = xSemaphoreCreateMutex();
    }
    if (s_fs_mutex) {
        xSemaphoreTake(s_fs_mutex, portMAX_DELAY);
    }
}

static void fs_unlock(void)
{
    if (s_fs_mutex) {
        xSemaphoreGive(s_fs_mutex);
    }
}

/* Static FIL objects in .bss to avoid FreeRTOS task stack overflow (FIL is ~550 bytes) */
static FIL s_drain_file;
static FIL s_http_file;
static FIL s_archive_file;
static FIL s_backlog_file;

void storage_log_init(void)
{
    if (!s_fs_mutex) {
        s_fs_mutex = xSemaphoreCreateMutex();
    }
    fs_lock();
    FRESULT res = f_mount(0, &s_fatfs);
    s_fs_mounted = (res == FR_OK);
    fs_unlock();

    if (s_fs_mounted) {
        storage_backlog_init();
    }
}

void storage_log_timer_step_10ms(void)
{
    disk_timerproc();
}

void storage_log_debug(const char *tag, const char *msg)
{
    if (!s_debug_dump_enabled || !msg) return;

    /* Format timestamp: real date/time if synced, else FreeRTOS tick uptime */
    char ts_str[24];
    telemetry_data_t t;
    if (intermcu_get_telemetry(&t) && strlen(t.present_time) >= 14 && strncmp(t.present_time, "20", 2) == 0 && t.present_time[2] != '0') {
        snprintf(ts_str, sizeof(ts_str), "%.4s-%.2s-%.2s %.2s:%.2s:%.2s",
                 t.present_time, t.present_time + 4, t.present_time + 6,
                 t.present_time + 8, t.present_time + 10, t.present_time + 12);
    } else {
        uint32_t ticks = xTaskGetTickCount();
        uint32_t sec = ticks / configTICK_RATE_HZ;
        uint32_t h = sec / 3600;
        uint32_t m = (sec % 3600) / 60;
        uint32_t s = sec % 60;
        uint32_t ms = (ticks % configTICK_RATE_HZ) * (1000 / configTICK_RATE_HZ);
        snprintf(ts_str, sizeof(ts_str), "T+%02u:%02u:%02u.%03u", (unsigned int)h, (unsigned int)m, (unsigned int)s, (unsigned int)ms);
    }

    /* Push string to ring buffer safely */
    char line[256];
    int len = snprintf(line, sizeof(line), "[%s] [%s] %s\r\n", ts_str, tag ? tag : "SYS", msg);
    if (len <= 0) return;
    if (len >= (int)sizeof(line)) len = sizeof(line) - 1;

    taskENTER_CRITICAL();

    for (int i = 0; i < len; i++) {
        uint16_t next_head = (s_head + 1) % RING_BUFFER_SIZE;
        if (next_head != s_tail) {
            s_ring_buffer[s_head] = line[i];
            s_head = next_head;
        } else {
            break; /* Buffer full, drop byte to avoid blocking */
        }
    }

    taskEXIT_CRITICAL();
}

void storage_log_http(const char *entry)
{
    if (!entry) return;

    fs_lock();
    /* Ensure volume is mounted */
    if (!s_fs_mounted) {
        f_mount(0, &s_fatfs);
        s_fs_mounted = true;
    }

    if (f_open(&s_http_file, HTTP_LOG_FILENAME, FA_OPEN_ALWAYS | FA_WRITE) == FR_OK) {
        f_lseek(&s_http_file, s_http_file.fsize);
        UINT bw;
        f_write(&s_http_file, entry, strlen(entry), &bw);
        f_write(&s_http_file, "\r\n", 2, &bw);
        f_close(&s_http_file);
    }
    fs_unlock();
}

void storage_log_drain_step(void)
{
    if (s_head == s_tail) return; /* No data to flush */

    fs_lock();
    /* Ensure FatFs object is registered */
    if (!s_fs_mounted) {
        f_mount(0, &s_fatfs);
        s_fs_mounted = true;
    }

    /* Open DEBUG.TXT in root of SD card */
    if (f_open(&s_drain_file, DEBUG_LOG_FILENAME, FA_OPEN_ALWAYS | FA_WRITE) == FR_OK) {
        f_lseek(&s_drain_file, s_drain_file.fsize);

        static char s_drain_buf[512];
        uint16_t drain_idx = 0;

        taskENTER_CRITICAL();
        while (s_tail != s_head && drain_idx < sizeof(s_drain_buf)) {
            s_drain_buf[drain_idx++] = s_ring_buffer[s_tail];
            s_tail = (s_tail + 1) % RING_BUFFER_SIZE;
        }
        taskEXIT_CRITICAL();

        if (drain_idx > 0) {
            UINT bw;
            f_write(&s_drain_file, s_drain_buf, drain_idx, &bw);
        }
        f_close(&s_drain_file);
    } else {
        /* If opening failed, SD card might have been pulled or not ready yet.
         * Drop up to 128 bytes if ring buffer is nearly full (>1800 bytes) so we don't stall. */
        taskENTER_CRITICAL();
        uint16_t count = (s_head >= s_tail) ? (s_head - s_tail) : (RING_BUFFER_SIZE - s_tail + s_head);
        if (count > (RING_BUFFER_SIZE - 200)) {
            s_tail = (s_tail + 128) % RING_BUFFER_SIZE;
        }
        taskEXIT_CRITICAL();
    }
    fs_unlock();
}

void storage_log_flush(void)
{
    for (int i = 0; i < 5; i++) {
        if (s_tail == s_head) break;
        storage_log_drain_step();
    }
}

bool storage_archive_slot(const char *date6, const char *record)
{
    if (!date6 || !record) return false;

    fs_lock();
    if (!s_fs_mounted) {
        f_mount(0, &s_fatfs);
        s_fs_mounted = true;
    }

    char filepath[32];
    f_mkdir("DATA");
    snprintf(filepath, sizeof(filepath), "DATA/%s.TXT", date6);

    bool ok = false;
    if (f_open(&s_archive_file, filepath, FA_OPEN_ALWAYS | FA_WRITE) == FR_OK) {
        f_lseek(&s_archive_file, s_archive_file.fsize);
        UINT bw;
        f_write(&s_archive_file, record, strlen(record), &bw);
        f_write(&s_archive_file, "\r\n", 2, &bw);
        f_close(&s_archive_file);
        ok = true;
    }
    fs_unlock();
    return ok;
}

/* =========================================================================
 *  Backlog Records Management on SD Card
 * ========================================================================= */

typedef struct __attribute__((packed)) {
    uint8_t sent;         /* 0 = unsent/pending, 1 = sent */
    char slot_time[16];   /* "20261104220000" */
    char payload[384];    /* HTTP payload string */
} storage_backlog_entry_t;

#define BACKLOG_FILENAME "BACKLOG.DAT"

void storage_backlog_init(void)
{
    if (!s_fs_mounted) {
        f_mount(0, &s_fatfs);
        s_fs_mounted = true;
    }
    uint32_t count = storage_backlog_get_pending_count();
    ui_set_backlog_count(count);
}

bool storage_backlog_save(const char *slot_time, const char *payload)
{
    if (!payload) return false;

    fs_lock();
    if (!s_fs_mounted) {
        f_mount(0, &s_fatfs);
        s_fs_mounted = true;
    }

    bool ok = false;
    if (f_open(&s_backlog_file, BACKLOG_FILENAME, FA_OPEN_ALWAYS | FA_WRITE) == FR_OK) {
        f_lseek(&s_backlog_file, s_backlog_file.fsize);

        storage_backlog_entry_t entry;
        memset(&entry, 0, sizeof(entry));
        entry.sent = 0; /* Unsent */
        if (slot_time) {
            strncpy(entry.slot_time, slot_time, sizeof(entry.slot_time) - 1);
        }
        strncpy(entry.payload, payload, sizeof(entry.payload) - 1);

        UINT bw;
        f_write(&s_backlog_file, &entry, sizeof(entry), &bw);
        f_close(&s_backlog_file);
        ok = true;
    }
    fs_unlock();

    if (ok) {
        ui_inc_backlog_count();
        storage_log_debug("BACKLOG", "Saved pending record to BACKLOG.DAT");
    }
    return ok;
}

uint32_t storage_backlog_get_pending_count(void)
{
    fs_lock();
    if (!s_fs_mounted) {
        f_mount(0, &s_fatfs);
        s_fs_mounted = true;
    }

    if (f_open(&s_backlog_file, BACKLOG_FILENAME, FA_OPEN_EXISTING | FA_READ) != FR_OK) {
        fs_unlock();
        return 0;
    }

    uint32_t count = 0;
    storage_backlog_entry_t entry;
    UINT br;

    while (f_read(&s_backlog_file, &entry, sizeof(entry), &br) == FR_OK && br == sizeof(entry)) {
        if (entry.sent == 0) {
            count++;
        }
    }
    f_close(&s_backlog_file);
    fs_unlock();
    return count;
}

bool storage_backlog_get_next_unsent(uint32_t start_offset, uint32_t *out_offset, char *out_payload, size_t max_len)
{
    if (!out_offset || !out_payload || max_len == 0) return false;

    fs_lock();
    if (!s_fs_mounted) {
        f_mount(0, &s_fatfs);
        s_fs_mounted = true;
    }

    if (f_open(&s_backlog_file, BACKLOG_FILENAME, FA_OPEN_EXISTING | FA_READ | FA_WRITE) != FR_OK) {
        fs_unlock();
        return false;
    }

    if (start_offset > 0) {
        if (f_lseek(&s_backlog_file, start_offset) != FR_OK) {
            f_close(&s_backlog_file);
            fs_unlock();
            return false;
        }
    }

    storage_backlog_entry_t entry;
    UINT br;
    uint32_t current_pos = s_backlog_file.fptr;

    while (f_read(&s_backlog_file, &entry, sizeof(entry), &br) == FR_OK && br == sizeof(entry)) {
        if (entry.sent == 0) {
            /* Guard: Discard legacy corrupted entries (e.g. from previous broken format with empty fields) */
            if (strstr(entry.payload, "temp=&") != NULL ||
                strstr(entry.payload, "bat_volt=&") != NULL ||
                strstr(entry.payload, "rainfall=&") != NULL ||
                strstr(entry.payload, "rec_time=2026-01-01,00:00") != NULL) {
                storage_log_debug("BACKLOG", "Discarding malformed legacy backlog record");
                f_lseek(&s_backlog_file, current_pos);
                uint8_t sent_flag = 1;
                UINT bw;
                f_write(&s_backlog_file, &sent_flag, 1, &bw);
                f_lseek(&s_backlog_file, current_pos + sizeof(entry));
                ui_dec_backlog_count();
                current_pos = s_backlog_file.fptr;
                continue;
            }

            *out_offset = current_pos;
            strncpy(out_payload, entry.payload, max_len - 1);
            out_payload[max_len - 1] = '\0';
            f_close(&s_backlog_file);
            fs_unlock();
            return true;
        }
        current_pos = s_backlog_file.fptr;
    }

    f_close(&s_backlog_file);
    fs_unlock();
    return false;
}

bool storage_backlog_mark_sent(uint32_t entry_offset)
{
    fs_lock();
    if (!s_fs_mounted) {
        f_mount(0, &s_fatfs);
        s_fs_mounted = true;
    }

    if (f_open(&s_backlog_file, BACKLOG_FILENAME, FA_OPEN_EXISTING | FA_WRITE) != FR_OK) {
        fs_unlock();
        return false;
    }

    if (f_lseek(&s_backlog_file, entry_offset) != FR_OK) {
        f_close(&s_backlog_file);
        fs_unlock();
        return false;
    }

    uint8_t sent_flag = 1;
    UINT bw;
    f_write(&s_backlog_file, &sent_flag, 1, &bw);
    f_close(&s_backlog_file);
    fs_unlock();

    ui_dec_backlog_count();
    return true;
}

void storage_backlog_cleanup_if_all_sent(void)
{
    if (storage_backlog_get_pending_count() == 0) {
        fs_lock();
        f_unlink(BACKLOG_FILENAME);
        fs_unlock();
        storage_log_debug("BACKLOG", "All backlogs sent, reclaimed BACKLOG.DAT");
    }
}

void storage_backlog_clear_all(void)
{
    fs_lock();
    f_unlink(BACKLOG_FILENAME);
    fs_unlock();
    ui_set_backlog_count(0);
    storage_log_debug("BACKLOG", "Backlog file erased (Erase Data)");
}
