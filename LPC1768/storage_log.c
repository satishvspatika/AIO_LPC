#include "storage_log.h"
#include "errlog.h"
#include "user_config.h"
#include "ui_display.h"
#include "intermcu_protocol.h"
#include "ff.h"
#include "fw_update.h"
#include "diskio.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <stdio.h>
#include <string.h>

#define RING_BUFFER_SIZE 1792 /* Trimmed from 2048 to leave the interrupt stack room */

#define BACKLOG_FILENAME "BACKLOG.DAT"

static FATFS s_fatfs;
static bool s_fs_mounted = false;
/* Until the card is mounted the switch isn't known: lines are kept in the ring meanwhile, and
 * thrown away if the log turns out to be off */
static volatile bool s_debug_dump_enabled = true;
static volatile bool s_debug_decided = false;

static char s_ring_buffer[RING_BUFFER_SIZE];
static volatile uint16_t s_head = 0;
static volatile uint16_t s_tail = 0;
static volatile uint32_t s_dropped_lines = 0; /* Lines lost because the ring buffer was full */
static uint32_t s_write_failures = 0;         /* Times the log file could not be written */
static int s_last_write_error = 0;            /* FatFs result of the last failure */
static bool s_log_dir_ready = false;
static portTickType s_next_log_retry_tick = 0;
#define LOG_RETRY_TICKS (5000 / portTICK_RATE_MS) /* With no card, don't keep retrying the card */
#define LOG_WRITE_RETRY_TICKS (500 / portTICK_RATE_MS)

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
/* Firmware files (fw_file_*) keep this one open across many calls while an update reads them. It
 * used s_backlog_file until v6.9.35: a slot touching the backlog meanwhile reopened that object
 * under the reader, and the update stopped with SD READ ERROR (bench, 2026-11-12 14:54). */
static FIL s_fw_file;

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
    storage_debug_log_decide();
}

/* SD_DEBUG_LOG_DEFAULT, or DEBUGLOG.ON on the card */
void storage_debug_log_decide(void)
{
    bool on = SD_DEBUG_LOG_DEFAULT;
    if (!on && s_fs_mounted) {
        fs_lock();
        if (f_open(&s_http_file, DEBUG_LOG_ON_FILENAME, FA_OPEN_EXISTING | FA_READ) == FR_OK) {
            f_close(&s_http_file);
            on = true;
        }
        fs_unlock();
    }
    taskENTER_CRITICAL();
    s_debug_dump_enabled = on;
    s_debug_decided = true;
    if (!on) {
        s_tail = s_head;        /* Boot lines kept so far are not wanted */
        s_dropped_lines = 0;
    }
    taskEXIT_CRITICAL();
}

bool storage_debug_log_on(void)
{
    return s_debug_dump_enabled;
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
    if (!msg) return;
    /* Errors also go to the flash error log, which keeps them even with the SD log switched off */
    bool is_error = tag && strcmp(tag, "ERROR") == 0;
    if (!s_debug_dump_enabled && !is_error) return;

    char ts_str[36];
    format_log_time(ts_str, sizeof(ts_str));

    /* Push string to ring buffer safely */
    char line[256];
    int len = format_log_line(line, sizeof(line), ts_str, tag, msg);
    if (len <= 0) return;
    if (is_error) {
        errlog_note(line, msg);
    }
    if (!s_debug_dump_enabled) return;

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
    if (!entry || !s_debug_dump_enabled) return;

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
        FRESULT res = f_close(&s_http_file);
        if (res != FR_OK) {
            s_write_failures++;
            s_last_write_error = (int)res;
        }
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

/* v6.9.31: written straight after the last log write before power-off. BIHAR's card dropped the
 * last data block it was given before power-off (while keeping the file size written after it), so
 * the log's last lines were lost at about half of all slots, even with 1 s to settle. This small
 * write makes the log block no longer the last one. The next boot logs what this file holds, which
 * shows whether this write itself survived. Fixed length, so no older text is left behind. */
void storage_power_off_mark(void)
{
    /* Written with the debug log off too: then the card's last write would otherwise be a state
     * file (LASTSLOT.DAT, FLASHSYN.DAT, LASTLOG.DAT) or SLOTS.CSV, which BIHAR's card could lose */
    char ts[36];
    char text[40];
    format_log_time(ts, sizeof(ts));
    snprintf(text, sizeof(text), "%-32.32s\r\n", ts);
    storage_state_save(POWER_OFF_MARK_FILENAME, text);
}

/* The mark the last power-off left, for the boot log */
void storage_log_power_off_mark(void)
{
    char text[40];
    char line[64];
    if (!storage_state_load(POWER_OFF_MARK_FILENAME, text, sizeof(text))) {
        return;
    }
    size_t n = strlen(text);
    while (n > 0 && (text[n - 1] == '\r' || text[n - 1] == '\n' || text[n - 1] == ' ')) {
        text[--n] = '\0';
    }
    snprintf(line, sizeof(line), "Last power-off mark: %s", text);
    storage_log_debug("BOOT", line);
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

/* A failed write keeps its lines in the buffer, to be written again. With no card, the oldest go
 * once the buffer is nearly full, so the newest lines are the ones kept. */
static void log_write_failed(FRESULT res, portTickType retry_ticks)
{
    s_write_failures++;
    s_last_write_error = (int)res;
    s_next_log_retry_tick = xTaskGetTickCount() + retry_ticks;

    taskENTER_CRITICAL();
    uint16_t count = (s_head >= s_tail) ? (s_head - s_tail) : (RING_BUFFER_SIZE - s_tail + s_head);
    if (count > (RING_BUFFER_SIZE - 200)) {
        /* At least 128 bytes, ending on a line ending so the rest are whole lines */
        for (uint16_t n = 0; s_tail != s_head; n++) {
            char c = s_ring_buffer[s_tail];
            s_tail = (s_tail + 1) % RING_BUFFER_SIZE;
            if (c == '\n') {
                s_dropped_lines++;
                if (n >= 128) {
                    break;
                }
            }
        }
    }
    taskEXIT_CRITICAL();
}

void storage_log_drain_step(void)
{
    if (s_head == s_tail) return; /* No data to flush */
    if (!s_debug_decided || !s_debug_dump_enabled) return;  /* Not written before the switch is read */
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

    FRESULT res = f_open(&s_drain_file, s_log_path, FA_OPEN_ALWAYS | FA_WRITE);
    if (res != FR_OK) {
        /* SD card might have been pulled or not ready yet */
        f_mount(0, &s_fatfs); /* Forces a fresh card start on the next try */
        log_write_failed(res, LOG_RETRY_TICKS);
        fs_unlock();
        return;
    }

    /* Only copied here: they leave the buffer once the card has taken them */
    static char s_drain_buf[512];
    uint16_t drain_idx = 0;
    uint16_t read_pos = s_tail;

    taskENTER_CRITICAL();
    while (read_pos != s_head && drain_idx < sizeof(s_drain_buf)) {
        s_drain_buf[drain_idx++] = s_ring_buffer[read_pos];
        read_pos = (read_pos + 1) % RING_BUFFER_SIZE;
    }
    taskEXIT_CRITICAL();

    /* Whole lines only, so the note below can't land inside one. A line is under 256 bytes. */
    uint16_t whole = drain_idx;
    while (whole > 0 && s_drain_buf[whole - 1] != '\n') {
        whole--;
    }
    if (whole > 0 && whole < drain_idx) {
        read_pos = (s_tail + whole) % RING_BUFFER_SIZE;
        drain_idx = whole;
    }

    UINT bw = 0;
    res = f_lseek(&s_drain_file, s_drain_file.fsize);
    if (res == FR_OK) {
        res = f_write(&s_drain_file, s_drain_buf, drain_idx, &bw);
    }
    if (res == FR_OK && bw != drain_idx) {
        res = FR_DENIED; /* Card is full */
    }

    /* Record anything the log itself lost, so gaps in it can be explained */
    uint32_t dropped = 0;
    uint32_t failures = s_write_failures;
    if (res == FR_OK) {
        taskENTER_CRITICAL();
        dropped = s_dropped_lines;
        s_dropped_lines = 0;
        taskEXIT_CRITICAL();
        if (dropped > 0 || failures > 0) {
            char note[88];
            UINT note_bw;
            int note_len = snprintf(note, sizeof(note), "[LOG] LOST %u lines (buffer full), %u SD write failures, last error %d\r\n",
                                    (unsigned int)dropped, (unsigned int)failures, s_last_write_error);
            if (note_len > 0) {
                f_write(&s_drain_file, note, (UINT)note_len, &note_bw);
            }
        }
    }

    /* The file size only reaches the card at close: until then nothing is really written */
    FRESULT close_res = f_close(&s_drain_file);
    if (res == FR_OK) {
        res = close_res;
    }

    if (res == FR_OK) {
        s_tail = read_pos;
        s_write_failures = 0;
    } else {
        taskENTER_CRITICAL();
        s_dropped_lines += dropped; /* Its note wasn't saved */
        taskEXIT_CRITICAL();
        log_write_failed(res, LOG_WRITE_RETRY_TICKS);
    }
    fs_unlock();
}

/* Runs before power is cut, so it can't wait out the retry delay */
void storage_log_flush(void)
{
    if (!s_debug_decided || !s_debug_dump_enabled) {
        return;     /* Nothing will be written: don't wait for it */
    }
    int failed_tries = 0;
    for (int i = 0; i < 16 && s_tail != s_head && failed_tries < 3; i++) {
        uint16_t before = s_tail;
        s_next_log_retry_tick = 0;
        storage_log_drain_step();
        if (s_tail == before) {
            failed_tries++;
            vTaskDelay(100 / portTICK_RATE_MS);
        }
    }
}

static bool s_card_full;

bool storage_card_full(void)
{
    return s_card_full;
}

/* v6.9.32 lengthened Bihar day-file names (DATA/001923_<date> to DATA/00001923_<date>). A day
 * file is one MET day, 08:45 to 08:30, named by the date it closes on: the first record after the
 * update renames that day's file, so the day stays in one file. Nothing happens if the old name
 * isn't there (the usual case) or the new one already is. */
void storage_archive_adopt(const char *day_file, const char *old_day_file)
{
    if (!day_file || !old_day_file || strcmp(day_file, old_day_file) == 0) return;
    char from[32], to[32];
    snprintf(from, sizeof(from), "DATA/%s.TXT", old_day_file);
    snprintf(to, sizeof(to), "DATA/%s.TXT", day_file);
    fs_lock();
    if (!s_fs_mounted) {
        f_mount(0, &s_fatfs);
        s_fs_mounted = true;
    }
    FRESULT r = f_rename(from, to);
    fs_unlock();
    if (r == FR_OK) {
        char line[80];
        snprintf(line, sizeof(line), "Day file %s renamed to %s (8-digit station ID)", from, to);
        storage_log_debug("SD", line);
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
    s_card_full = false;
    if (f_open(&s_archive_file, filepath, FA_OPEN_ALWAYS | FA_READ | FA_WRITE) == FR_OK) {
        UINT bw = 0, bw_end = 0;
        UINT len = (UINT)strlen(record);

        /* Power lost after writing a row but before noting it was copied brings the same row here
         * again. Only the newest row can be repeated that way, so only that one is compared. */
        char last_row[80];
        if (len + 2 <= sizeof(last_row) && s_archive_file.fsize >= len + 2) {
            UINT br = 0;
            f_lseek(&s_archive_file, s_archive_file.fsize - (len + 2));
            if (f_read(&s_archive_file, last_row, len + 2, &br) == FR_OK && br == len + 2 &&
                memcmp(last_row, record, len) == 0 && last_row[len] == '\r' && last_row[len + 1] == '\n') {
                ok = (f_close(&s_archive_file) == FR_OK);
                fs_unlock();
                return ok;
            }
        }

        DWORD start = s_archive_file.fsize;
        f_lseek(&s_archive_file, start);
        FRESULT r1 = f_write(&s_archive_file, record, len, &bw);
        FRESULT r2 = f_write(&s_archive_file, "\r\n", 2, &bw_end);
        s_card_full = (r1 == FR_OK && r2 == FR_OK && (bw != len || bw_end != 2));
        if (s_card_full) {
            /* A full card takes part of a row. Cut it back off, or the catch-up adds the whole row
             * after the broken one. */
            f_lseek(&s_archive_file, start);
            f_truncate(&s_archive_file);
        }
        FRESULT r3 = f_close(&s_archive_file);
        ok = (r1 == FR_OK && r2 == FR_OK && r3 == FR_OK && !s_card_full);
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
        /* Whole entries only (review, v6.9.51): a short write left on a full card would shift every
         * later entry, so none would read right and the file would be dropped as all sent. A part
         * entry, from now or from an earlier cut, is cut off before this one is added. */
        DWORD whole = (s_backlog_file.fsize / sizeof(storage_backlog_entry_t)) * sizeof(storage_backlog_entry_t);
        if (whole != s_backlog_file.fsize) {
            f_lseek(&s_backlog_file, whole);
            f_truncate(&s_backlog_file);
        }
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
        if (r1 == FR_OK && bw != sizeof(entry) && bw != 0) {
            f_lseek(&s_backlog_file, s_backlog_file.fsize - bw);
            f_truncate(&s_backlog_file);
        }
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
    FRESULT wr = f_write(&s_backlog_file, &sent_flag, 1, &bw);
    FRESULT cl = f_close(&s_backlog_file);
    fs_unlock();
    if (wr != FR_OK || bw != 1 || cl != FR_OK) {
        return false;
    }

    ui_dec_backlog_count();
    return true;
}

/* The server refused this record. The count sits in the entry's unused last slot_time byte, so older
 * files read as no strikes. Once set aside, the record goes to REJECTED.TXT to resend by hand, and
 * stays in DATA/ and flash. Returns true then. */
bool storage_backlog_note_rejected(uint32_t entry_offset, int strikes_allowed, const char *payload)
{
    const uint32_t strike_at = entry_offset + offsetof(storage_backlog_entry_t, slot_time) +
                               sizeof(((storage_backlog_entry_t *)0)->slot_time) - 1;
    bool set_aside = false;

    fs_lock();
    if (f_open(&s_backlog_file, BACKLOG_FILENAME, FA_OPEN_EXISTING | FA_READ | FA_WRITE) == FR_OK) {
        UINT n = 0;
        uint8_t strikes = 0;
        if (f_lseek(&s_backlog_file, strike_at) == FR_OK &&
            f_read(&s_backlog_file, &strikes, 1, &n) == FR_OK && n == 1) {
            strikes++;
            if (strikes >= strikes_allowed) {
                uint8_t aside_flag = 2;
                set_aside = (f_lseek(&s_backlog_file, entry_offset) == FR_OK &&
                             f_write(&s_backlog_file, &aside_flag, 1, &n) == FR_OK && n == 1);
            } else if (f_lseek(&s_backlog_file, strike_at) == FR_OK) {
                f_write(&s_backlog_file, &strikes, 1, &n);
            }
        }
        f_close(&s_backlog_file);
    }
    fs_unlock();

    if (set_aside) {
        ui_dec_backlog_count();
        storage_append_stamped(REJECTED_FILENAME, payload);
    }
    return set_aside;
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

/* Firmware files. The card is shared, so the lock is taken per read, and the backlog's file object
 * is free because an update can't start near a slot. */
bool fw_file_open(const char *name, uint32_t *out_size)
{
    fs_lock();
    if (!s_fs_mounted) {
        f_mount(0, &s_fatfs);
        s_fs_mounted = true;
    }
    bool ok = (f_open(&s_fw_file, name, FA_OPEN_EXISTING | FA_READ) == FR_OK);
    if (ok && out_size) {
        *out_size = (uint32_t)s_fw_file.fsize;
    }
    fs_unlock();
    return ok;
}

int fw_file_read(void *buf, uint32_t len)
{
    UINT got = 0;
    fs_lock();
    FRESULT r = f_read(&s_fw_file, buf, len, &got);
    fs_unlock();
    return (r == FR_OK) ? (int)got : -1;
}

void fw_file_close(void)
{
    fs_lock();
    f_close(&s_fw_file);
    fs_unlock();
}

void storage_state_delete(const char *name)
{
    fs_lock();
    if (!s_fs_mounted) {
        f_mount(0, &s_fatfs);
        s_fs_mounted = true;
    }
    f_unlink(name);
    fs_unlock();
}

/* A new station type starts fresh: its records, and what the old type had queued or remembered.
 * Debug logs, SLOTS.CSV and UPDATE.TXT stay, as they say what happened. Returns files removed. */
int storage_erase_station_data(void)
{
    static const char *const state_files[] = { LAST_SLOT_FILENAME, FLASH_SYNC_FILENAME, LAST_LOGGED_FILENAME, BACKLOG_FILENAME, REJECTED_FILENAME };
    int removed = 0;
    DIR dir;
    /* Zeroed: with long names on (_USE_LFN), f_readdir writes each name to info.lfname. Left as
     * stack garbage it wrote to a random address and the unit crashed at every boot until the
     * erase finished (1922 to KSNDMC_TWS, 2026-11-09). NULL means short names only, which is what
     * f_unlink below needs. */
    FILINFO info = { 0 };
    char path[24];

    fs_lock();
    if (!s_fs_mounted) {
        f_mount(0, &s_fatfs);
        s_fs_mounted = true;
    }
    for (size_t i = 0; i < sizeof(state_files) / sizeof(state_files[0]); i++) {
        if (f_unlink(state_files[i]) == FR_OK) {
            removed++;
        }
    }
    if (f_opendir(&dir, "DATA") == FR_OK) {
        /* Reading on from the same place after a delete is safe in FatFs, but restarting is simpler */
        for (int guard = 0; guard < 2000; guard++) {
            if (f_opendir(&dir, "DATA") != FR_OK || f_readdir(&dir, &info) != FR_OK) {
                break;
            }
            while (info.fname[0] == '.' ) {
                if (f_readdir(&dir, &info) != FR_OK) {
                    info.fname[0] = '\0';
                }
            }
            if (info.fname[0] == '\0') {
                break;
            }
            snprintf(path, sizeof(path), "DATA/%s", info.fname);
            if (f_unlink(path) != FR_OK) {
                break;
            }
            removed++;
        }
    }
    fs_unlock();
    return removed;
}

void storage_append_stamped(const char *name, const char *text)
{
    char ts_str[36];
    format_log_time(ts_str, sizeof(ts_str));
    fs_lock();
    if (!s_fs_mounted) {
        f_mount(0, &s_fatfs);
        s_fs_mounted = true;
    }
    if (f_open(&s_http_file, name, FA_OPEN_ALWAYS | FA_WRITE) == FR_OK) {
        UINT bw;
        char stamp[44];
        int n = snprintf(stamp, sizeof(stamp), "[%s] ", ts_str);
        f_lseek(&s_http_file, s_http_file.fsize);
        if (n > 0) {
            f_write(&s_http_file, stamp, (UINT)n, &bw);
        }
        f_write(&s_http_file, text, strlen(text), &bw);
        f_write(&s_http_file, "\r\n", 2, &bw);
        f_close(&s_http_file);
    }
    fs_unlock();
}

bool storage_errors_append(const char *text, unsigned repeats)
{
    bool ok = false;
    fs_lock();
    if (!s_fs_mounted) {
        f_mount(0, &s_fatfs);
        s_fs_mounted = true;
    }
    if (f_open(&s_http_file, ERRORS_FILENAME, FA_OPEN_ALWAYS | FA_WRITE) == FR_OK) {
        if (s_http_file.fsize >= 65536UL) {
            f_close(&s_http_file);
            f_unlink(ERRORS_OLD_FILENAME);
            f_rename(ERRORS_FILENAME, ERRORS_OLD_FILENAME);
            if (f_open(&s_http_file, ERRORS_FILENAME, FA_OPEN_ALWAYS | FA_WRITE) != FR_OK) {
                fs_unlock();
                return false;
            }
        }
        char note[40] = "";
        if (repeats > 0) {
            snprintf(note, sizeof(note), "(previous error again %u times) ", repeats);
        }
        UINT bw1 = 0, bw2 = 0, bw3 = 0;
        f_lseek(&s_http_file, s_http_file.fsize);
        FRESULT r1 = f_write(&s_http_file, note, strlen(note), &bw1);
        FRESULT r2 = f_write(&s_http_file, text, strlen(text), &bw2);
        FRESULT r3 = f_write(&s_http_file, "\r\n", 2, &bw3);
        FRESULT r4 = f_close(&s_http_file);
        ok = (r1 == FR_OK && r2 == FR_OK && r3 == FR_OK && r4 == FR_OK && bw3 == 2);
    }
    fs_unlock();
    return ok;
}

void storage_backlog_clear_all(void)
{
    fs_lock();
    f_unlink(BACKLOG_FILENAME);
    fs_unlock();
    ui_set_backlog_count(0);
    storage_log_debug("BACKLOG", "Backlog file erased (Erase Data)");
}
