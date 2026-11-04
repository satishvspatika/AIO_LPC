#include "storage_log.h"
#include "user_config.h"
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

void storage_log_init(void)
{
    FRESULT res = f_mount(0, &s_fatfs);
    s_fs_mounted = (res == FR_OK);
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
