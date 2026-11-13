#include "errlog.h"
#include "storage_log.h"
#include "user_config.h"
#include "LPC17xx.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>

/* Sectors 9-11 (4 KB each): spare space after the bootloader, outside both firmware slots and the
 * record log. A fixed ring of 48 entries, so it can't grow into anything else. */
#define ERRLOG_BASE         0x00009000UL
#define ERRLOG_FIRST_SECTOR 9
#define PAGE_SIZE           256
#define PAGES_PER_SECTOR    16
#define TOTAL_PAGES         48
#define ERRLOG_MAGIC        0x474F4C45UL /* 'ELOG' */
#define WRITES_PER_BOOT     8           /* A fault that repeats can't wear the flash out */
#define SD_SYNC_EVERY_SEC   30

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t seq;
    uint16_t repeats;   /* Times the previous entry happened again before this one */
    uint16_t crc;
    char text[PAGE_SIZE - 12];
} errlog_page_t;

_Static_assert(sizeof(errlog_page_t) == PAGE_SIZE, "error log page must be one flash page");

#define IAP_LOCATION 0x1FFF1FF1UL
typedef void (*iap_entry_t)(unsigned long[], unsigned long[]);
static const iap_entry_t iap_call = (iap_entry_t)IAP_LOCATION;
static const unsigned long s_cclk_khz[] = { 100000, 398000 };
static unsigned long s_cmd[5];
static unsigned long s_res[5];

/* Interrupts stay off during flash commands: code can't run from flash meanwhile */
static bool iap_run(void)
{
    __disable_irq();
    iap_call(s_cmd, s_res);
    __enable_irq();
    return s_res[0] == 0;
}

static bool iap_prepare(uint32_t sector)
{
    s_cmd[0] = 50;
    s_cmd[1] = sector;
    s_cmd[2] = sector;
    return iap_run();
}

static bool iap_erase(uint32_t sector)
{
    for (size_t c = 0; c < sizeof(s_cclk_khz) / sizeof(s_cclk_khz[0]); c++) {
        if (!iap_prepare(sector)) {
            continue;
        }
        s_cmd[0] = 52;
        s_cmd[1] = sector;
        s_cmd[2] = sector;
        s_cmd[3] = s_cclk_khz[c];
        if (iap_run()) {
            return true;
        }
    }
    return false;
}

/* From main RAM: a caller's stack (in AHB SRAM) can be a source the boot ROM refuses (fw_app.c) */
static uint32_t s_page_src[PAGE_SIZE / 4];

static bool iap_write(uint32_t page, const void *src)
{
    memcpy(s_page_src, src, PAGE_SIZE);
    src = s_page_src;
    uint32_t sector = ERRLOG_FIRST_SECTOR + page / PAGES_PER_SECTOR;
    for (size_t c = 0; c < sizeof(s_cclk_khz) / sizeof(s_cclk_khz[0]); c++) {
        if (!iap_prepare(sector)) {
            continue;
        }
        s_cmd[0] = 51;
        s_cmd[1] = ERRLOG_BASE + page * PAGE_SIZE;
        s_cmd[2] = (unsigned long)src;
        s_cmd[3] = PAGE_SIZE;
        s_cmd[4] = s_cclk_khz[c];
        if (iap_run()) {
            return true;
        }
    }
    return false;
}

static const errlog_page_t *page_at(uint32_t page)
{
    return (const errlog_page_t *)(ERRLOG_BASE + page * PAGE_SIZE);
}

static uint16_t page_crc(const errlog_page_t *p)
{
    const uint8_t *b = (const uint8_t *)p;
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < PAGE_SIZE; i++) {
        if (i == offsetof(errlog_page_t, crc) || i == offsetof(errlog_page_t, crc) + 1) {
            continue;
        }
        crc ^= (uint16_t)(b[i] << 8);
        for (int k = 0; k < 8; k++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static bool page_valid(const errlog_page_t *p)
{
    return p->magic == ERRLOG_MAGIC && p->crc == page_crc(p);
}

static bool page_blank(uint32_t page)
{
    const uint32_t *w = (const uint32_t *)(ERRLOG_BASE + page * PAGE_SIZE);
    for (size_t i = 0; i < PAGE_SIZE / 4; i++) {
        if (w[i] != 0xFFFFFFFFUL) {
            return false;
        }
    }
    return true;
}

static uint32_t hash_text(const char *s)
{
    uint32_t h = 5381;
    while (*s) {
        h = h * 33 + (uint8_t)*s++;
    }
    return h;
}

static uint32_t s_next_page;
static uint32_t s_next_seq = 1;
static uint8_t s_writes;
static uint32_t s_last_hash;        /* Newest entry written, to count repeats instead */
static uint16_t s_last_repeats;
static char s_pending[96];
static uint32_t s_pending_hash;
static uint16_t s_pending_repeats;
static volatile bool s_has_pending;
static uint32_t s_synced;
static bool s_sync_loaded;
static uint8_t s_sync_timer;

void errlog_init(void)
{
    uint32_t newest_seq = 0, newest_page = 0;
    for (uint32_t i = 0; i < TOTAL_PAGES; i++) {
        const errlog_page_t *p = page_at(i);
        if (page_valid(p) && p->seq > newest_seq) {
            newest_seq = p->seq;
            newest_page = i;
        }
    }
    s_next_seq = newest_seq + 1;
    s_next_page = newest_seq ? (newest_page + 1) % TOTAL_PAGES : 0;
}

void errlog_note(const char *line, const char *msg)
{
    if (!line || !msg) {
        return;
    }
    uint32_t h = hash_text(msg);
    taskENTER_CRITICAL();
    if (s_has_pending && h == s_pending_hash) {
        s_pending_repeats++;
    } else if (!s_has_pending && h == s_last_hash) {
        s_last_repeats++;
    } else if (!s_has_pending) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n')) {
            n--;
        }
        if (n >= sizeof(s_pending)) {
            n = sizeof(s_pending) - 1;
        }
        memcpy(s_pending, line, n);
        s_pending[n] = '\0';
        s_pending_hash = h;
        s_pending_repeats = 0;
        s_has_pending = true;
    }
    /* Otherwise one is already waiting: this one stays in the SD log only */
    taskEXIT_CRITICAL();
}

/* Already in flash today? The LPC1768 restarts every slot, so counting repeats in RAM can't stop a
 * fault that lasts (dead RTC on 001903: the same line 96 times a day, 2026-11-13) from filling the
 * 48 entries and pushing real faults out. Same message, same date: once a day is enough. Entries
 * look like "v6.9.37 [2026-11-13 00:00:32 +2s] [ERROR] text". */
static bool already_logged_today(const char *text)
{
    const char *when = strchr(text, '[');
    const char *msg = strstr(text, "] [ERROR] ");
    if (!when || !msg || when[1] != '2' || when[5] != '-') {
        return false;   /* No date yet (clock not known): always kept */
    }
    for (uint32_t i = 0; i < TOTAL_PAGES; i++) {
        const errlog_page_t *p = page_at(i);
        if (!page_valid(p)) {
            continue;
        }
        const char *p_when = strchr(p->text, '[');
        const char *p_msg = strstr(p->text, "] [ERROR] ");
        if (p_when && p_msg && strncmp(p_when, when, 11) == 0 && strcmp(p_msg, msg) == 0) {
            return true;
        }
    }
    return false;
}

/* False when nothing was written (already logged today) */
static bool write_pending(void)
{
    errlog_page_t page;
    memset(&page, 0xFF, sizeof(page));
    page.magic = ERRLOG_MAGIC;
    page.seq = s_next_seq;

    taskENTER_CRITICAL();
    page.repeats = s_last_repeats;
    snprintf(page.text, sizeof(page.text), "%s %s", FIRMWARE_VERSION, s_pending);
    s_last_hash = s_pending_hash;
    s_last_repeats = s_pending_repeats;
    s_has_pending = false;
    taskEXIT_CRITICAL();

    /* Only the text after the time is compared, so the time itself can't make it new. The repeats
     * counted for the previous entry stay with it, as RAM, until a different error is written. */
    if (already_logged_today(page.text)) {
        return false;
    }

    page.crc = page_crc(&page);

    /* Starting a sector: it holds the oldest entries, which make way */
    for (int tries = 0; tries < 2; tries++) {
        uint32_t pg = s_next_page;
        if ((pg % PAGES_PER_SECTOR) == 0 && !page_blank(pg)) {
            iap_erase(ERRLOG_FIRST_SECTOR + pg / PAGES_PER_SECTOR);
        }
        s_next_page = (pg + 1) % TOTAL_PAGES;
        if (page_blank(pg) && iap_write(pg, &page)) {
            s_next_seq++;
            return true;
        }
        /* Damaged page (power lost mid-write): skip it */
    }
    return true;
}

/* New entries to ERRORS.TXT, oldest first, a few at a time */
static void sync_to_card(void)
{
    char line[16];
    if (!s_sync_loaded) {
        s_synced = 0;
        if (storage_state_load(ERRORS_SYNC_FILENAME, line, sizeof(line)) && strlen(line) >= 10) {
            s_synced = (uint32_t)atol(line);
        }
        if (s_synced >= s_next_seq) {
            s_synced = 0;   /* A different unit's card, or the log was cleared */
        }
        s_sync_loaded = true;
    }
    uint32_t start = s_synced;
    for (int n = 0; n < 8; n++) {
        const errlog_page_t *best = NULL;
        for (uint32_t i = 0; i < TOTAL_PAGES; i++) {
            const errlog_page_t *p = page_at(i);
            if (page_valid(p) && p->seq > s_synced && (!best || p->seq < best->seq)) {
                best = p;
            }
        }
        if (!best || !storage_errors_append(best->text, best->repeats)) {
            break;
        }
        s_synced = best->seq;
    }
    if (s_synced != start) {
        snprintf(line, sizeof(line), "%010lu\r\n", (unsigned long)s_synced);
        storage_state_save(ERRORS_SYNC_FILENAME, line);
    }
}

bool errlog_idle(void)
{
    return !s_has_pending;
}

void errlog_step(void)
{
    if (s_has_pending) {
        if (s_writes < WRITES_PER_BOOT) {
            if (write_pending()) {
                s_writes++;
            }
        } else {
            taskENTER_CRITICAL();
            s_has_pending = false;
            taskEXIT_CRITICAL();
        }
    }
    if (++s_sync_timer >= SD_SYNC_EVERY_SEC) {
        s_sync_timer = 0;
        sync_to_card();
    }
}
