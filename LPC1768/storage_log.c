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
static volatile uint32_t s_dropped_lines = 0; /* Lines lost because the ring buffer was full */
static uint32_t s_write_failures = 0;         /* Times the log file could not be written */
static bool s_log_dir_ready = false;
static portTickType s_next_log_retry_tick = 0;
#define LOG_RETRY_TICKS (5000 / portTICK_RATE_MS) /* With no card, don't keep retrying the card */

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
}

void storage_log_mount(void)
{
    fs_lock();
    if (!s_fs_mounted) {
        s_fs_mounted = (f_mount(0, &s_fatfs) == FR_OK);
    }
    fs_unlock();

    if (s_fs_mounted) {
        storage_backlog_init();
    }
}

void storage_log_timer_step_10ms(void)
{
    disk_timerproc();
}

/* Builds one log line. Modem replies arrive as several lines: joining them here means every
 * line in the log starts with its time. Returns the length. */
static int format_log_line(char *line, size_t size, const char *ts, const char *tag, const char *msg)
{
    int prefix = snprintf(line, size, "[%s] [%s] ", ts, tag ? tag : "SYS");
    if (prefix < 0 || (size_t)prefix >= size - 8) {
        return 0;
    }

    size_t limit = size - 6; /* Room for "..." and the line ending */
    size_t len = (size_t)prefix;
    bool truncated = false;
    for (const char *p = msg; *p; p++) {
        char c = *p;
        if (c == '\r' || c == '\n') {
            c = ' ';
        } else if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) {
            continue;
        }
        if (c == ' ' && (len == (size_t)prefix || line[len - 1] == ' ')) {
            continue;
        }
        if (len >= limit) {
            truncated = true;
            break;
        }
        line[len++] = c;
    }
    while (len > (size_t)prefix && line[len - 1] == ' ') {
        len--;
    }
    if (truncated) {
        memcpy(&line[len], "...", 3);
        len += 3;
    }
    line[len++] = '\r';
    line[len++] = '\n';
    line[len] = '\0';
    return (int)len;
}

/* Real date and time if the clock is valid, else time since boot. Always with uptime. */
static void format_log_time(char *ts_str, size_t size)
{
    telemetry_data_t t;
    if (intermcu_get_telemetry(&t) && strlen(t.present_time) >= 14 && strncmp(t.present_time, "20", 2) == 0 && t.present_time[2] != '0') {
        snprintf(ts_str, size, "%.4s-%.2s-%.2s %.2s:%.2s:%.2s +%us",
                 t.present_time, t.present_time + 4, t.present_time + 6,
                 t.present_time + 8, t.present_time + 10, t.present_time + 12,
                 (unsigned int)(xTaskGetTickCount() / configTICK_RATE_HZ));
    } else {
        uint32_t ticks = xTaskGetTickCount();
        uint32_t sec = ticks / configTICK_RATE_HZ;
        uint32_t h = sec / 3600;
        uint32_t m = (sec % 3600) / 60;
        uint32_t s = sec % 60;
        uint32_t ms = (ticks % configTICK_RATE_HZ) * (1000 / configTICK_RATE_HZ);
        snprintf(ts_str, size, "T+%02u:%02u:%02u.%03u", (unsigned int)h, (unsigned int)m, (unsigned int)s, (unsigned int)ms);
    }
}

void storage_log_debug(const char *tag, const char *msg)
{
    if (!s_debug_dump_enabled || !msg) return;

    char ts_str[36];
    format_log_time(ts_str, sizeof(ts_str));

    /* Push string to ring buffer safely */
    char line[256];
    int len = format_log_line(line, sizeof(line), ts_str, tag, msg);
    if (len <= 0) return;

    taskENTER_CRITICAL();

    /* Store whole lines only, so a full buffer can't leave half a line in the log */
    uint16_t used = (s_head >= s_tail) ? (s_head - s_tail) : (RING_BUFFER_SIZE - s_tail + s_head);
    if ((RING_BUFFER_SIZE - 1 - used) >= (uint16_t)len) {
        for (int i = 0; i < len; i++) {
            s_ring_buffer[s_head] = line[i];
            s_head = (s_head + 1) % RING_BUFFER_SIZE;
        }
    } else {
        s_dropped_lines++;
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
        char ts_str[36];
        char stamp[44];
        format_log_time(ts_str, sizeof(ts_str));
        int stamp_len = snprintf(stamp, sizeof(stamp), "[%s] ", ts_str);
        if (stamp_len > 0) {
            f_write(&s_http_file, stamp, (UINT)stamp_len, &bw);
        }
        f_write(&s_http_file, entry, strlen(entry), &bw);
        f_write(&s_http_file, "\r\n", 2, &bw);
        f_close(&s_http_file);
    }
    fs_unlock();
}

void storage_log_slot(const char *csv_line)
{
    if (!csv_line) return;

    fs_lock();
    if (!s_fs_mounted) {
        f_mount(0, &s_fatfs);
        s_fs_mounted = true;
    }

    if (f_open(&s_http_file, SLOT_LOG_FILENAME, FA_OPEN_ALWAYS | FA_WRITE) == FR_OK) {
        UINT bw;
        if (s_http_file.fsize == 0) {
            static const char header[] = "time,slot,result,reason,http_attempts,csq_dbm,duration_sec,battery_v,boot,backlog\r\n";
            f_write(&s_http_file, header, sizeof(header) - 1, &bw);
        }
        f_lseek(&s_http_file, s_http_file.fsize);
        f_write(&s_http_file, csv_line, strlen(csv_line), &bw);
        f_write(&s_http_file, "\r\n", 2, &bw);
        f_close(&s_http_file);
    } else {
        s_write_failures++;
    }
    fs_unlock();
}

bool storage_state_load(const char *filename, char *out, size_t size)
{
    if (!filename || !out || size == 0) return false;

    bool ok = false;
    fs_lock();
    if (!s_fs_mounted) {
        f_mount(0, &s_fatfs);
        s_fs_mounted = true;
    }
    if (f_open(&s_http_file, filename, FA_OPEN_EXISTING | FA_READ) == FR_OK) {
        UINT br = 0;
        if (f_read(&s_http_file, out, size - 1, &br) == FR_OK && br > 0) {
            out[br] = '\0';
            ok = true;
        }
        f_close(&s_http_file);
    }
    fs_unlock();
    return ok;
}

bool storage_state_save(const char *filename, const char *text)
{
    if (!filename || !text) return false;

    bool ok = false;
    fs_lock();
    if (!s_fs_mounted) {
        f_mount(0, &s_fatfs);
        s_fs_mounted = true;
    }
    /* No truncate: a power cut can't leave an empty file, only the previous or new text */
    if (f_open(&s_http_file, filename, FA_OPEN_ALWAYS | FA_WRITE) == FR_OK) {
        UINT bw = 0;
        UINT len = (UINT)strlen(text);
        FRESULT r1 = f_write(&s_http_file, text, len, &bw);
        FRESULT r2 = f_close(&s_http_file);
        ok = (r1 == FR_OK && r2 == FR_OK && bw == len);
    }
    fs_unlock();
    return ok;
}

bool storage_sd_is_ok(void)
{
    static const char probe[] = "OK\r\n";

    if (storage_state_save(SD_CHECK_FILENAME, probe)) {
        return true;
    }

    /* A card refitted since it failed needs a fresh start before it can answer */
    fs_lock();
    f_mount(0, &s_fatfs);
    fs_unlock();
    return storage_state_save(SD_CHECK_FILENAME, probe);
}

/* One log file per day: LOG/YYMMDD.TXT, or DEBUG.TXT until the clock is valid */
static void current_log_path(char *path, size_t size)
{
    telemetry_data_t t;
    if (intermcu_get_telemetry(&t) && strlen(t.present_time) >= 8 && strncmp(t.present_time, "20", 2) == 0 && t.present_time[2] != '0') {
        if (!s_log_dir_ready) {
            FRESULT res = f_mkdir("LOG");
            s_log_dir_ready = (res == FR_OK || res == FR_EXIST);
        }
        if (s_log_dir_ready) {
            snprintf(path, size, "LOG/%.6s.TXT", t.present_time + 2);
            return;
        }
    }
    /* Until the folder exists, keep logging somewhere rather than losing it */
    snprintf(path, size, "%s", DEBUG_LOG_FILENAME);
}

void storage_log_drain_step(void)
{
    if (s_head == s_tail) return; /* No data to flush */
    if (s_next_log_retry_tick != 0) {
        if ((long)(xTaskGetTickCount() - s_next_log_retry_tick) < 0) {
            return;
        }
        s_next_log_retry_tick = 0;
    }

    fs_lock();
    /* Ensure FatFs object is registered */
    if (!s_fs_mounted) {
        f_mount(0, &s_fatfs);
        s_fs_mounted = true;
    }

    static char s_log_path[24];
    current_log_path(s_log_path, sizeof(s_log_path));

    if (f_open(&s_drain_file, s_log_path, FA_OPEN_ALWAYS | FA_WRITE) == FR_OK) {
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

        /* Record anything the log itself lost, so gaps in it can be explained */
        taskENTER_CRITICAL();
        uint32_t dropped = s_dropped_lines;
        s_dropped_lines = 0;
        taskEXIT_CRITICAL();
        if (dropped > 0 || s_write_failures > 0) {
            char note[64];
            UINT bw;
            int note_len = snprintf(note, sizeof(note), "[LOG] LOST %u lines (buffer full), %u SD write failures\r\n",
                                    (unsigned int)dropped, (unsigned int)s_write_failures);
            s_write_failures = 0;
            if (note_len > 0) {
                f_write(&s_drain_file, note, (UINT)note_len, &bw);
            }
        }
        f_close(&s_drain_file);
    } else {
        /* If opening failed, SD card might have been pulled or not ready yet.
         * Drop up to 128 bytes if ring buffer is nearly full (>1800 bytes) so we don't stall. */
        s_write_failures++;
        s_next_log_retry_tick = xTaskGetTickCount() + LOG_RETRY_TICKS;
        f_mount(0, &s_fatfs); /* Forces a fresh card start on the next try */
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
    for (int i = 0; i < 8; i++) {
        if (s_tail == s_head) break;
        storage_log_drain_step();
    }
}

bool storage_archive_slot(const char *day_file, const char *record)
{
    if (!day_file || !record) return false;

    fs_lock();
    if (!s_fs_mounted) {
        f_mount(0, &s_fatfs);
        s_fs_mounted = true;
    }

    char filepath[32];
    f_mkdir("DATA");
    snprintf(filepath, sizeof(filepath), "DATA/%s.TXT", day_file);

    /* Data reaches the card at close, so its result counts: with no card-detect pin this is
     * how a pulled card shows up */
    bool ok = false;
    if (f_open(&s_archive_file, filepath, FA_OPEN_ALWAYS | FA_WRITE) == FR_OK) {
        f_lseek(&s_archive_file, s_archive_file.fsize);
        UINT bw = 0, bw_end = 0;
        UINT len = (UINT)strlen(record);
        FRESULT r1 = f_write(&s_archive_file, record, len, &bw);
        FRESULT r2 = f_write(&s_archive_file, "\r\n", 2, &bw_end);
        FRESULT r3 = f_close(&s_archive_file);
        ok = (r1 == FR_OK && r2 == FR_OK && r3 == FR_OK && bw == len && bw_end == 2);
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

        UINT bw = 0;
        FRESULT r1 = f_write(&s_backlog_file, &entry, sizeof(entry), &bw);
        FRESULT r2 = f_close(&s_backlog_file);
        ok = (r1 == FR_OK && r2 == FR_OK && bw == sizeof(entry));
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
