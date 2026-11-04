#include "storage_log.h"
#include "user_config.h"
#include "ff.h"
#include "diskio.h"
#include <stdio.h>
#include <string.h>

#define RING_BUFFER_SIZE 1024

static FATFS s_fatfs;
static bool s_fs_mounted = false;
static bool s_debug_dump_enabled = SDC_DEBUG_DUMP_DEFAULT;

static char s_ring_buffer[RING_BUFFER_SIZE];
static uint16_t s_head = 0;
static uint16_t s_tail = 0;

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

    /* Push string to ring buffer */
    char line[128];
    int len = snprintf(line, sizeof(line), "[%s] %s\r\n", tag ? tag : "SYS", msg);
    for (int i = 0; i < len; i++) {
        uint16_t next_head = (s_head + 1) % RING_BUFFER_SIZE;
        if (next_head != s_tail) {
            s_ring_buffer[s_head] = line[i];
            s_head = next_head;
        } else {
            break; /* Buffer full, drop record to prevent CPU stall */
        }
    }
}

void storage_log_http(const char *entry)
{
    if (!s_fs_mounted || !entry) return;

    FIL file;
    if (f_open(&file, HTTP_LOG_FILENAME, FA_OPEN_ALWAYS | FA_WRITE) == FR_OK) {
        f_lseek(&file, file.fsize);
        UINT bw;
        f_write(&file, entry, strlen(entry), &bw);
        f_write(&file, "\r\n", 2, &bw);
        f_close(&file);
    }
}

void storage_log_drain_step(void)
{
    if (!s_fs_mounted) return;

    static bool s_checked_trigger = false;
    if (!s_checked_trigger) {
        s_checked_trigger = true;
        FILINFO fno;
        if (f_stat(DEBUG_TRIGGER_FILENAME, &fno) == FR_OK) {
            s_debug_dump_enabled = true;
        }
    }

    if (s_head == s_tail) return;

    FIL file;
    if (f_open(&file, DEBUG_LOG_FILENAME, FA_OPEN_ALWAYS | FA_WRITE) == FR_OK) {
        f_lseek(&file, file.fsize);

        char drain_buf[128];
        uint16_t drain_idx = 0;

        while (s_tail != s_head && drain_idx < sizeof(drain_buf) - 1) {
            drain_buf[drain_idx++] = s_ring_buffer[s_tail];
            s_tail = (s_tail + 1) % RING_BUFFER_SIZE;
        }

        UINT bw;
        f_write(&file, drain_buf, drain_idx, &bw);
        f_close(&file);
    }
}

bool storage_archive_slot(const char *date6, const char *record)
{
    if (!s_fs_mounted || !date6 || !record) return false;

    char filepath[32];
    f_mkdir("/DATA");
    snprintf(filepath, sizeof(filepath), "/DATA/%s.TXT", date6);

    FIL file;
    if (f_open(&file, filepath, FA_OPEN_ALWAYS | FA_WRITE) == FR_OK) {
        f_lseek(&file, file.fsize);
        UINT bw;
        f_write(&file, record, strlen(record), &bw);
        f_write(&file, "\r\n", 2, &bw);
        f_close(&file);
        return true;
    }
    return false;
}
