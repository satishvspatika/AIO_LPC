#include "fw_journal.h"

#define J_MAGIC 0x4C4E524AUL /* 'JRNL' */
#define J_WRITE_TRIES 8

/* CRC-32, the same as zlib, one bit at a time: small, and only used on short records here */
static uint32_t record_crc(const fw_record_t *r)
{
    const uint8_t *p = (const uint8_t *)r;
    uint32_t crc = 0xFFFFFFFFUL;
    for (uint32_t i = 0; i < 12; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
        }
    }
    return ~crc;
}

static const fw_record_t *page_record(uint32_t page)
{
    return (const fw_record_t *)fw_hal_ptr(FW_JOURNAL_BASE + page * FW_JOURNAL_PAGE);
}

static bool page_valid(uint32_t page)
{
    fw_record_t r;
    const uint8_t *src = (const uint8_t *)page_record(page);
    uint8_t *dst = (uint8_t *)&r;
    for (uint32_t i = 0; i < sizeof(r); i++) {
        dst[i] = src[i];
    }
    return r.magic == J_MAGIC && r.crc == record_crc(&r);
}

static bool page_blank(uint32_t page)
{
    const uint8_t *p = fw_hal_ptr(FW_JOURNAL_BASE + page * FW_JOURNAL_PAGE);
    for (uint32_t i = 0; i < FW_JOURNAL_PAGE; i++) {
        if (p[i] != 0xFF) {
            return false;
        }
    }
    return true;
}

/* Index of the newest valid page, or -1 */
static int32_t newest_page(uint32_t *out_seq)
{
    int32_t best = -1;
    uint32_t best_seq = 0;
    for (uint32_t i = 0; i < FW_JOURNAL_PAGES; i++) {
        if (page_valid(i)) {
            uint32_t seq = page_record(i)->seq;
            if (best < 0 || seq > best_seq) {
                best = (int32_t)i;
                best_seq = seq;
            }
        }
    }
    if (out_seq) {
        *out_seq = best_seq;
    }
    return best;
}

bool fw_journal_read(fw_record_t *rec)
{
    int32_t page = newest_page((uint32_t *)0);
    if (page < 0) {
        rec->magic = 0;
        rec->seq = 0;
        rec->type = FW_J_NONE;
        rec->op = FW_OP_NONE;
        rec->arg = 0;
        return false;
    }
    const uint8_t *src = (const uint8_t *)page_record((uint32_t)page);
    uint8_t *dst = (uint8_t *)rec;
    for (uint32_t i = 0; i < sizeof(*rec); i++) {
        dst[i] = src[i];
    }
    return true;
}

bool fw_journal_append(fw_j_type_t type, fw_op_t op, uint8_t arg)
{
    uint32_t seq = 0;
    int32_t newest = newest_page(&seq);
    uint32_t page = (newest < 0) ? 0 : (((uint32_t)newest + 1) % FW_JOURNAL_PAGES);
    uint32_t pages_per_sector = FW_JOURNAL_SECTOR_SIZE / FW_JOURNAL_PAGE;

    union {
        uint8_t bytes[FW_JOURNAL_PAGE];
        uint32_t align;
    } buf;
    fw_record_t rec;
    rec.magic = J_MAGIC;
    rec.seq = seq + 1;
    rec.type = (uint8_t)type;
    rec.op = (uint8_t)op;
    rec.arg = arg;
    rec.reserved = 0xFF;
    rec.crc = record_crc(&rec);
    for (uint32_t i = 0; i < FW_JOURNAL_PAGE; i++) {
        buf.bytes[i] = 0xFF;
    }
    const uint8_t *src = (const uint8_t *)&rec;
    for (uint32_t i = 0; i < sizeof(rec); i++) {
        buf.bytes[i] = src[i];
    }

    for (uint32_t tries = 0; tries < FW_JOURNAL_PAGES + J_WRITE_TRIES; tries++, page = (page + 1) % FW_JOURNAL_PAGES) {
        /* Entering a sector: clear it, unless it holds the current state */
        if ((page % pages_per_sector) == 0) {
            uint32_t sector_index = page / pages_per_sector;
            bool holds_newest = (newest >= 0) && (((uint32_t)newest / pages_per_sector) == sector_index);
            bool blank = true;
            for (uint32_t i = 0; i < pages_per_sector && blank; i++) {
                blank = page_blank(page + i);
            }
            if (!blank && !holds_newest && !fw_hal_erase(FW_JOURNAL_SECTOR + sector_index)) {
                continue;
            }
        }
        if (!page_blank(page)) {
            continue;
        }
        if (!fw_hal_program(FW_JOURNAL_BASE + page * FW_JOURNAL_PAGE, buf.bytes, FW_JOURNAL_PAGE)) {
            continue;
        }
        if (page_valid(page) && page_record(page)->seq == rec.seq) {
            return true;
        }
    }
    return false;
}
