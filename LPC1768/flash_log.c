#include "flash_log.h"
#include <string.h>

#define PAGE_SIZE       256
#define SECTOR_SIZE     0x8000UL
#define FIRST_SECTOR    25
#define SECTOR_COUNT    5
#define TOTAL_PAGES     ((SECTOR_COUNT * SECTOR_SIZE) / PAGE_SIZE)
#define PAGES_PER_SECTOR (SECTOR_SIZE / PAGE_SIZE)

#define LOG_MAGIC       0x474F4C46UL /* 'FLOG' */
#define TYPE_RECORD     1
#define TYPE_PENDING    2
#define TYPE_SENT       3
#define TYPE_OUTAGE     4   /* slot_id = first record the server did not get */
#define TYPE_RESUMED    5   /* slot_id = first record it got again */
#define TYPE_DRAINED    6   /* slot_id = seq up to which nothing is left to resend */
#define TYPE_STRIKE     7   /* slot_id = queued record the server refused */
#define MAX_OUTAGE_MARKERS 16
#define WRITE_TRIES     8

_Static_assert(sizeof(flash_log_page_t) == PAGE_SIZE, "flash log page must be exactly one flash page");

#ifdef FLASH_LOG_HOST_TEST
/* Simulated flash for tests on the host */
uint8_t g_flash_sim[SECTOR_COUNT * SECTOR_SIZE];
int g_flash_erases[SECTOR_COUNT];
int g_flash_fail_next_writes = 0;
int g_flash_torn_next_write = 0;
#define LOG_BASE ((uintptr_t)g_flash_sim)

static bool iap_erase_sector(uint32_t index)
{
    memset(g_flash_sim + index * SECTOR_SIZE, 0xFF, SECTOR_SIZE);
    g_flash_erases[index]++;
    return true;
}

static bool iap_write_page(uint32_t page, const void *src)
{
    if (g_flash_torn_next_write > 0) {
        g_flash_torn_next_write--;
        memcpy(g_flash_sim + page * PAGE_SIZE, src, 64); /* Power lost part way through */
        return false;
    }
    if (g_flash_fail_next_writes > 0) {
        g_flash_fail_next_writes--;
        return false;
    }
    memcpy(g_flash_sim + page * PAGE_SIZE, src, PAGE_SIZE);
    return true;
}
#else
#include "LPC17xx.h"

/* LPC1768 flash map: sector 16 starts at 0x10000, then 32 KB per sector up to sector 29 */
#define LOG_BASE        (0x00010000UL + (FIRST_SECTOR - 16) * SECTOR_SIZE)
_Static_assert((0x00010000UL + (FIRST_SECTOR - 16) * SECTOR_SIZE) == 0x00058000UL, "sector 24 is at 0x50000");
_Static_assert((0x00010000UL + (FIRST_SECTOR - 16 + SECTOR_COUNT) * SECTOR_SIZE) == 0x00080000UL, "log must end at the top of flash");
#define IAP_LOCATION    0x1FFF1FF1UL

typedef void (*iap_entry_t)(unsigned long[], unsigned long[]);
static const iap_entry_t iap_call = (iap_entry_t)IAP_LOCATION;
static const unsigned long s_cclk_khz[] = { 100000, 398000 };
static unsigned long s_iap_cmd[5];
static unsigned long s_iap_res[5];

/* Interrupts stay off during flash commands: code can't run from flash meanwhile */
static bool iap_run(void)
{
    __disable_irq();
    iap_call(s_iap_cmd, s_iap_res);
    __enable_irq();
    return s_iap_res[0] == 0;
}

static bool iap_prepare(uint32_t sector)
{
    s_iap_cmd[0] = 50;
    s_iap_cmd[1] = sector;
    s_iap_cmd[2] = sector;
    return iap_run();
}

static bool iap_erase_sector(uint32_t index)
{
    uint32_t sector = FIRST_SECTOR + index;
    for (size_t c = 0; c < sizeof(s_cclk_khz) / sizeof(s_cclk_khz[0]); c++) {
        if (!iap_prepare(sector)) {
            continue;
        }
        s_iap_cmd[0] = 52;
        s_iap_cmd[1] = sector;
        s_iap_cmd[2] = sector;
        s_iap_cmd[3] = s_cclk_khz[c];
        if (iap_run()) {
            return true;
        }
    }
    return false;
}

/* The flash write takes its data from main RAM: a caller's stack (in AHB SRAM) can be a source the boot
 * ROM refuses (see fw_app.c, s_iap_src) */
static uint32_t s_page_src[PAGE_SIZE / 4];

static bool iap_write_page(uint32_t page, const void *src)
{
    memcpy(s_page_src, src, PAGE_SIZE);
    src = s_page_src;
    uint32_t sector = FIRST_SECTOR + page / PAGES_PER_SECTOR;
    for (size_t c = 0; c < sizeof(s_cclk_khz) / sizeof(s_cclk_khz[0]); c++) {
        if (!iap_prepare(sector)) {
            continue;
        }
        s_iap_cmd[0] = 51;
        s_iap_cmd[1] = LOG_BASE + page * PAGE_SIZE;
        s_iap_cmd[2] = (unsigned long)src;
        s_iap_cmd[3] = PAGE_SIZE;
        s_iap_cmd[4] = s_cclk_khz[c];
        if (iap_run()) {
            return true;
        }
    }
    return false;
}
#endif

static uint32_t s_next_page = 0;
static uint32_t s_next_seq = 1;
static uint32_t s_undelivered_from = 0; /* 0: the newest record reached the server */
static uint32_t s_newest_outage = 0;    /* First record of the newest outage */
static uint32_t s_drained_seq = 0;

static const flash_log_page_t *oldest_pending(uint32_t *out_count);

static const flash_log_page_t *page_at(uint32_t page)
{
    return (const flash_log_page_t *)(LOG_BASE + page * PAGE_SIZE);
}

static uint16_t page_crc(const flash_log_page_t *p)
{
    const uint8_t *bytes = (const uint8_t *)p;
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < PAGE_SIZE; i++) {
        if (i == offsetof(flash_log_page_t, crc) || i == offsetof(flash_log_page_t, crc) + 1) {
            continue;
        }
        crc ^= (uint16_t)(bytes[i] << 8);
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static bool page_valid(const flash_log_page_t *p)
{
    return p->magic == LOG_MAGIC && p->crc == page_crc(p);
}

static bool page_blank(uint32_t page)
{
    const uint32_t *words = (const uint32_t *)(LOG_BASE + page * PAGE_SIZE); /* Pages are 256-byte aligned */
    for (size_t i = 0; i < PAGE_SIZE / sizeof(uint32_t); i++) {
        if (words[i] != 0xFFFFFFFFUL) {
            return false;
        }
    }
    return true;
}

void flash_log_init(void)
{
    uint32_t newest_seq = 0;
    uint32_t newest_page = TOTAL_PAGES - 1;

    for (uint32_t i = 0; i < TOTAL_PAGES; i++) {
        const flash_log_page_t *p = page_at(i);
        if (page_valid(p) && p->seq >= newest_seq) {
            newest_seq = p->seq;
            newest_page = i;
        }
    }
    s_next_seq = newest_seq + 1;
    s_next_page = (newest_page + 1) % TOTAL_PAGES;

    uint32_t newest_marker = 0;
    uint32_t newest_drained = 0;
    s_undelivered_from = 0;
    s_newest_outage = 0;
    s_drained_seq = 0;
    for (uint32_t i = 0; i < TOTAL_PAGES; i++) {
        const flash_log_page_t *p = page_at(i);
        if (!page_valid(p)) {
            continue;
        }
        if ((p->type == TYPE_OUTAGE || p->type == TYPE_RESUMED) && p->seq > newest_marker) {
            newest_marker = p->seq;
            s_undelivered_from = (p->type == TYPE_OUTAGE) ? (uint32_t)p->slot_id : 0;
        }
        if (p->type == TYPE_OUTAGE && (uint32_t)p->slot_id > s_newest_outage) {
            s_newest_outage = (uint32_t)p->slot_id;
        }
        if (p->type == TYPE_DRAINED && p->seq > newest_drained) {
            newest_drained = p->seq;
            s_drained_seq = (uint32_t)p->slot_id;
        }
    }
}

/* Page ready to be written: erases the next sector on entering it, skips torn pages */
static bool prepare_next_page(void)
{
    for (uint32_t tries = 0; tries < TOTAL_PAGES; tries++) {
        if ((s_next_page % PAGES_PER_SECTOR) == 0) {
            uint32_t sector_index = s_next_page / PAGES_PER_SECTOR;
            bool sector_blank = true;
            for (uint32_t i = 0; i < PAGES_PER_SECTOR && sector_blank; i++) {
                sector_blank = page_blank(s_next_page + i);
            }
            if (!sector_blank && !iap_erase_sector(sector_index)) {
                s_next_page = (s_next_page + PAGES_PER_SECTOR) % TOTAL_PAGES;
                continue;
            }
        }
        if (page_blank(s_next_page)) {
            return true;
        }
        s_next_page = (s_next_page + 1) % TOTAL_PAGES;
    }
    return false;
}

static uint32_t write_page(uint8_t type, int32_t slot_id, const char *record, const char *payload)
{
    /* On the stack: flash commands need the source in RAM, and word aligned */
    flash_log_page_t page __attribute__((aligned(4)));
    memset(&page, 0, sizeof(page));
    page.magic = LOG_MAGIC;
    page.type = type;
    page.slot_id = slot_id;
    if (record) {
        strncpy(page.record, record, sizeof(page.record) - 1);
    }
    if (payload && strlen(payload) < sizeof(page.payload)) {
        strcpy(page.payload, payload);
    }

    for (int tries = 0; tries < WRITE_TRIES; tries++) {
        if (!prepare_next_page()) {
            return 0;
        }
        page.seq = s_next_seq;
        page.crc = page_crc(&page);

        uint32_t target = s_next_page;
        bool written = iap_write_page(target, &page);
        s_next_page = (target + 1) % TOTAL_PAGES;
        if (written && memcmp(page_at(target), &page, PAGE_SIZE) == 0) {
            return s_next_seq++;
        }
    }
    return 0;
}

uint32_t flash_log_append(int32_t slot_id, const char *record, const char *payload)
{
    return write_page(TYPE_RECORD, slot_id, record, payload);
}

const flash_log_page_t *flash_log_next_record(uint32_t after_seq)
{
    const flash_log_page_t *best = NULL;
    for (uint32_t i = 0; i < TOTAL_PAGES; i++) {
        const flash_log_page_t *p = page_at(i);
        if (page_valid(p) && p->type == TYPE_RECORD && p->seq > after_seq && (!best || p->seq < best->seq)) {
            best = p;
        }
    }
    return best;
}

/* Only the start and end of an outage take a page, so normal running costs no record space */
void flash_log_note_delivery(uint32_t record_seq, bool delivered)
{
    if (record_seq == 0) {
        return;
    }
    if (delivered && s_undelivered_from != 0) {
        if (write_page(TYPE_RESUMED, (int32_t)record_seq, NULL, NULL) != 0) {
            s_undelivered_from = 0;
        }
    } else if (!delivered && s_undelivered_from == 0) {
        if (write_page(TYPE_OUTAGE, (int32_t)record_seq, NULL, NULL) != 0) {
            s_undelivered_from = record_seq;
            s_newest_outage = record_seq;
        }
    }
}

bool flash_log_outage_unsettled(void)
{
    return s_undelivered_from == 0 && s_newest_outage > s_drained_seq;
}

void flash_log_note_drained(void)
{
    if (flash_log_outage_unsettled() && oldest_pending(NULL) == NULL) {
        uint32_t newest = flash_log_newest_seq();
        if (write_page(TYPE_DRAINED, (int32_t)newest, NULL, NULL) != 0) {
            s_drained_seq = newest;
        }
    }
}

static bool queued_before(uint32_t record_seq)
{
    for (uint32_t i = 0; i < TOTAL_PAGES; i++) {
        const flash_log_page_t *p = page_at(i);
        if (p->magic == LOG_MAGIC && p->type == TYPE_PENDING && (uint32_t)p->slot_id == record_seq) {
            return true;
        }
    }
    return false;
}

/* The card holding the queue has gone, but flash still has what those outages left unsent. Queues
 * them here. Only runs when a card has failed, so normal running pays nothing for this. */
uint32_t flash_log_adopt_undelivered(void)
{
    struct { uint32_t ref; uint8_t type; } markers[MAX_OUTAGE_MARKERS]; /* RAM is short: not static */
    uint32_t count = 0;
    uint32_t adopted = 0;
    uint32_t newest = flash_log_newest_seq();

    if (!flash_log_outage_unsettled()) {
        return 0;
    }
    for (uint32_t i = 0; i < TOTAL_PAGES && count < MAX_OUTAGE_MARKERS; i++) {
        const flash_log_page_t *p = page_at(i);
        if (page_valid(p) && (p->type == TYPE_OUTAGE || p->type == TYPE_RESUMED)) {
            markers[count].ref = (uint32_t)p->slot_id;
            markers[count].type = p->type;
            count++;
        }
    }
    for (uint32_t i = 0; i < TOTAL_PAGES; i++) {
        const flash_log_page_t *p = page_at(i);
        if (!page_valid(p) || p->type != TYPE_RECORD || p->seq <= s_drained_seq ||
            p->seq > newest || p->payload[0] == '\0') {
            continue;
        }
        uint32_t best_ref = 0;
        uint8_t best_type = 0;
        for (uint32_t m = 0; m < count; m++) {
            if (markers[m].ref <= p->seq && markers[m].ref >= best_ref) {
                best_ref = markers[m].ref;
                best_type = markers[m].type;
            }
        }
        if (best_type == TYPE_OUTAGE && !queued_before(p->seq) &&
            write_page(TYPE_PENDING, (int32_t)p->seq, NULL, NULL) != 0) {
            adopted++;
        }
    }
    if (write_page(TYPE_DRAINED, (int32_t)newest, NULL, NULL) != 0) {
        s_drained_seq = newest;
    }
    return adopted;
}

/* No more than a few pages per bad record, and only when the server says no */
bool flash_log_note_rejected(uint32_t record_seq, int strikes_allowed)
{
    int strikes = 1;
    for (uint32_t i = 0; i < TOTAL_PAGES; i++) {
        const flash_log_page_t *p = page_at(i);
        if (page_valid(p) && p->type == TYPE_STRIKE && (uint32_t)p->slot_id == record_seq) {
            strikes++;
        }
    }
    if (strikes >= strikes_allowed) {
        flash_log_mark_sent(record_seq);
        return true;
    }
    write_page(TYPE_STRIKE, (int32_t)record_seq, NULL, NULL);
    return false;
}

/* How many of the newest records, up to limit, were made with no network, newest first */
int flash_log_no_network_run(int limit)
{
    int run = 0;
    uint32_t below = 0xFFFFFFFFUL;
    while (run < limit) {
        const flash_log_page_t *best = NULL;
        for (uint32_t i = 0; i < TOTAL_PAGES; i++) {
            const flash_log_page_t *p = page_at(i);
            if (page_valid(p) && p->type == TYPE_RECORD && p->seq < below && (!best || p->seq > best->seq)) {
                best = p;
            }
        }
        if (!best || strstr(best->record, ",-113,") == NULL) {
            break;
        }
        run++;
        below = best->seq;
    }
    return run;
}

const flash_log_page_t *flash_log_last_delivered(void)
{
    if (s_undelivered_from == 0) {
        return flash_log_newest_record();
    }
    const flash_log_page_t *best = NULL;
    for (uint32_t i = 0; i < TOTAL_PAGES; i++) {
        const flash_log_page_t *p = page_at(i);
        if (page_valid(p) && p->type == TYPE_RECORD && p->seq < s_undelivered_from &&
            (!best || p->seq > best->seq)) {
            best = p;
        }
    }
    return best;
}

const flash_log_page_t *flash_log_newest_record(void)
{
    const flash_log_page_t *best = NULL;
    for (uint32_t i = 0; i < TOTAL_PAGES; i++) {
        const flash_log_page_t *p = page_at(i);
        if (page_valid(p) && p->type == TYPE_RECORD && (!best || p->seq > best->seq)) {
            best = p;
        }
    }
    return best;
}

uint32_t flash_log_newest_seq(void)
{
    return s_next_seq - 1;
}

int32_t flash_log_newest_slot(void)
{
    int32_t slot = -1;
    uint32_t seq = 0;
    for (uint32_t i = 0; i < TOTAL_PAGES; i++) {
        const flash_log_page_t *p = page_at(i);
        if (page_valid(p) && p->type == TYPE_RECORD && p->seq > seq) {
            seq = p->seq;
            slot = p->slot_id;
        }
    }
    return slot;
}

uint32_t flash_log_record_count(void)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < TOTAL_PAGES; i++) {
        const flash_log_page_t *p = page_at(i);
        if (page_valid(p) && p->type == TYPE_RECORD) {
            count++;
        }
    }
    return count;
}

static const flash_log_page_t *find_record(uint32_t seq)
{
    for (uint32_t i = 0; i < TOTAL_PAGES; i++) {
        const flash_log_page_t *p = page_at(i);
        if (page_valid(p) && p->type == TYPE_RECORD && p->seq == seq) {
            return p;
        }
    }
    return NULL;
}

static bool marked_sent(uint32_t record_seq)
{
    for (uint32_t i = 0; i < TOTAL_PAGES; i++) {
        const flash_log_page_t *p = page_at(i);
        if (page_valid(p) && p->type == TYPE_SENT && (uint32_t)p->slot_id == record_seq) {
            return true;
        }
    }
    return false;
}

bool flash_log_mark_pending(uint32_t record_seq)
{
    const flash_log_page_t *rec = find_record(record_seq);
    if (!rec || rec->payload[0] == '\0') {
        return false;
    }
    return write_page(TYPE_PENDING, (int32_t)record_seq, NULL, NULL) != 0;
}

bool flash_log_mark_sent(uint32_t record_seq)
{
    return write_page(TYPE_SENT, (int32_t)record_seq, NULL, NULL) != 0;
}

/* Finds the oldest pending record still held and not yet sent */
static const flash_log_page_t *oldest_pending(uint32_t *out_count)
{
    const flash_log_page_t *best = NULL;
    uint32_t count = 0;
    for (uint32_t i = 0; i < TOTAL_PAGES; i++) {
        const flash_log_page_t *p = page_at(i);
        if (!page_valid(p) || p->type != TYPE_PENDING) {
            continue;
        }
        uint32_t ref = (uint32_t)p->slot_id;
        if (marked_sent(ref)) {
            continue;
        }
        const flash_log_page_t *rec = find_record(ref);
        if (!rec || rec->payload[0] == '\0') {
            continue;
        }
        count++;
        if (!best || rec->seq < best->seq) {
            best = rec;
        }
    }
    if (out_count) {
        *out_count = count;
    }
    return best;
}

bool flash_log_next_pending(uint32_t *out_seq, char *out_payload, size_t size)
{
    const flash_log_page_t *rec = oldest_pending(NULL);
    if (!rec || !out_seq || !out_payload || size == 0) {
        return false;
    }
    *out_seq = rec->seq;
    strncpy(out_payload, rec->payload, size - 1);
    out_payload[size - 1] = '\0';
    return true;
}

uint32_t flash_log_pending_count(void)
{
    uint32_t count = 0;
    oldest_pending(&count);
    return count;
}

void flash_log_erase_all(void)
{
    for (uint32_t i = 0; i < SECTOR_COUNT; i++) {
        iap_erase_sector(i);
    }
    s_next_page = 0;
    s_next_seq = 1;
    s_undelivered_from = 0;
    s_newest_outage = 0;
    s_drained_seq = 0;
}
