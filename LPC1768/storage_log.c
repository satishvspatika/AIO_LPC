#include "storage_log.h"
#include "user_config.h"
#include "ui_display.h"
#include "ff.h"
#include "diskio.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

#define RING_BUFFER_SIZE 2048

static FATFS s_fatfs;
static bool s_fs_mounted = false;
static bool s_debug_dump_enabled = SDC_DEBUG_DUMP_DEFAULT;

static char s_ring_buffer[RING_BUFFER_SIZE];
static volatile uint16_t s_head = 0;
static volatile uint16_t s_tail = 0;

/* Static FIL objects in .bss to avoid FreeRTOS task stack overflow (FIL is ~550 bytes) */
static FIL s_drain_file;
static FIL s_http_file;
static FIL s_archive_file;
static FIL s_backlog_file;

void storage_log_init(void)
{
    FRESULT res = f_mount(0, &s_fatfs);
    s_fs_mounted = (res == FR_OK);
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

    /* Push string to ring buffer safely */
    char line[160];
    int len = snprintf(line, sizeof(line), "[%s] %s\r\n", tag ? tag : "SYS", msg);
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
}

void storage_log_drain_step(void)
{
    if (s_head == s_tail) return; /* No data to flush */

    /* Ensure FatFs object is registered */
    if (!s_fs_mounted) {
        f_mount(0, &s_fatfs);
        s_fs_mounted = true;
    }

    /* Open DEBUG.TXT in root of SD card */
    if (f_open(&s_drain_file, DEBUG_LOG_FILENAME, FA_OPEN_ALWAYS | FA_WRITE) == FR_OK) {
        f_lseek(&s_drain_file, s_drain_file.fsize);

        static char s_drain_buf[256];
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
}

bool storage_archive_slot(const char *date6, const char *record)
{
    if (!date6 || !record) return false;

    if (!s_fs_mounted) {
        f_mount(0, &s_fatfs);
        s_fs_mounted = true;
    }

    char filepath[32];
    f_mkdir("DATA");
    snprintf(filepath, sizeof(filepath), "DATA/%s.TXT", date6);

    if (f_open(&s_archive_file, filepath, FA_OPEN_ALWAYS | FA_WRITE) == FR_OK) {
        f_lseek(&s_archive_file, s_archive_file.fsize);
        UINT bw;
        f_write(&s_archive_file, record, strlen(record), &bw);
        f_write(&s_archive_file, "\r\n", 2, &bw);
        f_close(&s_archive_file);
        return true;
    }
    return false;
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

    if (!s_fs_mounted) {
        f_mount(0, &s_fatfs);
        s_fs_mounted = true;
    }

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

        ui_inc_backlog_count();
        storage_log_debug("BACKLOG", "Saved pending record to BACKLOG.DAT");
        return true;
    }
    return false;
}

uint32_t storage_backlog_get_pending_count(void)
{
    if (!s_fs_mounted) {
        f_mount(0, &s_fatfs);
        s_fs_mounted = true;
    }

    if (f_open(&s_backlog_file, BACKLOG_FILENAME, FA_OPEN_EXISTING | FA_READ) != FR_OK) {
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
    return count;
}

bool storage_backlog_get_next_unsent(uint32_t start_offset, uint32_t *out_offset, char *out_payload, size_t max_len)
{
    if (!out_offset || !out_payload || max_len == 0) return false;

    if (!s_fs_mounted) {
        f_mount(0, &s_fatfs);
        s_fs_mounted = true;
    }

    if (f_open(&s_backlog_file, BACKLOG_FILENAME, FA_OPEN_EXISTING | FA_READ) != FR_OK) {
        return false;
    }

    if (start_offset > 0) {
        if (f_lseek(&s_backlog_file, start_offset) != FR_OK) {
            f_close(&s_backlog_file);
            return false;
        }
    }

    storage_backlog_entry_t entry;
    UINT br;
    uint32_t current_pos = s_backlog_file.fptr;

    while (f_read(&s_backlog_file, &entry, sizeof(entry), &br) == FR_OK && br == sizeof(entry)) {
        if (entry.sent == 0) {
            *out_offset = current_pos;
            strncpy(out_payload, entry.payload, max_len - 1);
            out_payload[max_len - 1] = '\0';
            f_close(&s_backlog_file);
            return true;
        }
        current_pos = s_backlog_file.fptr;
    }

    f_close(&s_backlog_file);
    return false;
}

bool storage_backlog_mark_sent(uint32_t entry_offset)
{
    if (!s_fs_mounted) {
        f_mount(0, &s_fatfs);
        s_fs_mounted = true;
    }

    if (f_open(&s_backlog_file, BACKLOG_FILENAME, FA_OPEN_EXISTING | FA_WRITE) != FR_OK) {
        return false;
    }

    if (f_lseek(&s_backlog_file, entry_offset) != FR_OK) {
        f_close(&s_backlog_file);
        return false;
    }

    uint8_t sent_flag = 1;
    UINT bw;
    f_write(&s_backlog_file, &sent_flag, 1, &bw);
    f_close(&s_backlog_file);

    ui_dec_backlog_count();
    return true;
}

void storage_backlog_cleanup_if_all_sent(void)
{
    if (storage_backlog_get_pending_count() == 0) {
        f_unlink(BACKLOG_FILENAME);
        storage_log_debug("BACKLOG", "All backlogs sent, reclaimed BACKLOG.DAT");
    }
}

void storage_backlog_clear_all(void)
{
    f_unlink(BACKLOG_FILENAME);
    ui_set_backlog_count(0);
    storage_log_debug("BACKLOG", "Backlog file erased (Erase Data)");
}
