#include "fw_update.h"
#include "boot/fw_layout.h"
#include "boot/fw_journal.h"
#include <string.h>

#define MAX_LINE_CHARS  96  /* 32 data bytes; objcopy writes 16 */
#define READ_CHUNK      128
#define INFO_SIZE       sizeof(fw_image_info_t)
#define INFO_CRC_OFFSET 12

/* ---- CRC-32 (same as zlib), a bit at a time: small, and fast enough for this ---- */
static uint32_t crc_byte(uint32_t crc, uint8_t b)
{
    crc ^= b;
    for (int i = 0; i < 8; i++) {
        crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
    }
    return crc;
}

/* ---- The image as it goes past, byte by byte ---- */
typedef struct {
    uint32_t next_offset;
    uint32_t crc;
    uint8_t head[36];       /* Vectors 0-8 */
    uint32_t info_offset;
    uint8_t info[sizeof(fw_image_info_t)];
    uint8_t page[256];      /* Only used when writing */
    uint32_t page_fill;
    bool writing;
    bool flash_failed;
} image_state_t;

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool flush_page(image_state_t *s)
{
    if (s->page_fill == 0) {
        return true;
    }
    memset(&s->page[s->page_fill], 0xFF, sizeof(s->page) - s->page_fill);
    uint32_t page_offset = (s->next_offset - 1) & ~0xFFUL;
    uint32_t addr = FW_SLOT_B_BASE + page_offset;
    s->page_fill = 0;
    if (!fw_hal_program(addr, s->page, sizeof(s->page))) {
        return false;
    }
    return memcmp(fw_hal_ptr(addr), s->page, sizeof(s->page)) == 0;
}

/* One byte of the image, at its offset in the slot */
static void image_byte(image_state_t *s, uint8_t b)
{
    uint32_t off = s->next_offset;

    if (off < sizeof(s->head)) {
        s->head[off] = b;
        if (off == sizeof(s->head) - 1) {
            s->info_offset = get_u32(&s->head[FW_VECTOR_IMAGE_INFO * 4]) - FW_SLOT_A_BASE;
        }
    }

    bool have_info_pos = off >= sizeof(s->head);
    bool in_info = have_info_pos && off >= s->info_offset && (off - s->info_offset) < INFO_SIZE;
    if (in_info) {
        s->info[off - s->info_offset] = b;
    }
    bool in_crc = in_info && (off - s->info_offset) >= INFO_CRC_OFFSET && (off - s->info_offset) < (INFO_CRC_OFFSET + 4);
    s->crc = crc_byte(s->crc, in_crc ? 0 : b);

    s->next_offset = off + 1;

    if (s->writing) {
        s->page[s->page_fill++] = b;
        if (s->page_fill == sizeof(s->page)) {
            if (!flush_page(s)) {
                s->flash_failed = true;
            }
        }
    }
}

/* ---- Intel HEX, a line at a time ---- */
static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

typedef struct {
    uint32_t base;
    bool ended;
} hex_state_t;

static fw_result_t hex_line(hex_state_t *h, image_state_t *s, const char *line, uint32_t len)
{
    uint8_t rec[37];

    if (len == 0) {
        return FW_OK; /* Blank line */
    }
    if (h->ended) {
        return FW_E_BAD_LINE; /* Nothing belongs after the end */
    }
    if (line[0] != ':' || len < 11 || (len & 1) == 0 || ((len - 1) / 2) > sizeof(rec)) {
        return FW_E_BAD_LINE;
    }
    uint32_t n = (len - 1) / 2;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < n; i++) {
        int hi = hex_digit(line[1 + i * 2]);
        int lo = hex_digit(line[2 + i * 2]);
        if (hi < 0 || lo < 0) {
            return FW_E_BAD_LINE;
        }
        rec[i] = (uint8_t)((hi << 4) | lo);
        sum = (uint8_t)(sum + rec[i]);
    }
    if (sum != 0 || rec[0] != n - 5) {
        return FW_E_BAD_LINE;
    }

    switch (rec[3]) {
    case 0: {
        uint32_t addr = h->base + (((uint32_t)rec[1] << 8) | rec[2]);
        if (addr < FW_SLOT_A_BASE || (addr + rec[0]) > (FW_SLOT_A_BASE + FW_SLOT_SIZE)) {
            return FW_E_BAD_ADDRESS;
        }
        if ((addr - FW_SLOT_A_BASE) != s->next_offset) {
            return FW_E_GAP;
        }
        for (uint32_t i = 0; i < rec[0]; i++) {
            image_byte(s, rec[4 + i]);
        }
        return s->flash_failed ? FW_E_FLASH : FW_OK;
    }
    case 1:
        h->ended = true;
        return FW_OK;
    case 2:
        if (rec[0] != 2) return FW_E_BAD_LINE;
        h->base = (((uint32_t)rec[4] << 8) | rec[5]) << 4;
        return FW_OK;
    case 4:
        if (rec[0] != 2) return FW_E_BAD_LINE;
        h->base = (((uint32_t)rec[4] << 8) | rec[5]) << 16;
        return FW_OK;
    case 3:
    case 5:
        return FW_OK; /* Start address: the image's own vectors decide that */
    default:
        return FW_E_BAD_LINE;
    }
}

static fw_result_t read_file(const char *name, image_state_t *s, const char *stage, fw_progress_fn progress)
{
    uint32_t size = 0;
    if (!fw_file_open(name, &size)) {
        return FW_E_NOT_FOUND;
    }

    hex_state_t h = { 0, false };
    char line[MAX_LINE_CHARS + 1];
    char chunk[READ_CHUNK];
    uint32_t line_len = 0, done = 0;
    bool line_too_long = false;
    unsigned last_percent = 101;
    fw_result_t result = FW_OK;

    for (;;) {
        int got = fw_file_read(chunk, sizeof(chunk));
        if (got < 0) {
            result = FW_E_READ;
            break;
        }
        if (got == 0) {
            break;
        }
        for (int i = 0; i < got && result == FW_OK; i++) {
            char c = chunk[i];
            if (c == '\n' || c == '\r') {
                result = line_too_long ? FW_E_BAD_LINE : hex_line(&h, s, line, line_len);
                line_len = 0;
                line_too_long = false;
            } else if (line_len < MAX_LINE_CHARS) {
                line[line_len++] = c;
            } else {
                line_too_long = true;
            }
        }
        if (result != FW_OK) {
            break;
        }
        done += (uint32_t)got;
        unsigned percent = (size > 0) ? (unsigned)((done / 16) * 100 / ((size / 16) + 1)) : 0;
        if (progress && percent != last_percent) {
            progress(stage, percent);
            last_percent = percent;
        }
    }
    /* A last line with no line ending */
    if (result == FW_OK && line_len > 0) {
        result = line_too_long ? FW_E_BAD_LINE : hex_line(&h, s, line, line_len);
    }
    fw_file_close();

    if (result == FW_OK && !h.ended) {
        result = FW_E_NO_END;
    }
    return result;
}

/* Is what went past a complete, undamaged image for this unit? */
static fw_result_t judge_image(const image_state_t *s, uint32_t expected_profile, bool check_profile, fw_file_info_t *out)
{
    uint32_t sp = get_u32(&s->head[0]);
    uint32_t reset = get_u32(&s->head[4]);
    if (s->next_offset < sizeof(s->head) || sp < 0x10000000UL || sp > 0x10008000UL ||
        reset < FW_SLOT_A_BASE || reset >= (FW_SLOT_A_BASE + FW_SLOT_SIZE) ||
        s->info_offset < sizeof(s->head) || (s->info_offset + INFO_SIZE) > s->next_offset) {
        return FW_E_NOT_AN_IMAGE;
    }
    if (get_u32(&s->info[0]) != FW_IMAGE_MAGIC) {
        return FW_E_NOT_AN_IMAGE;
    }
    if (get_u32(&s->info[4]) != FW_BOARD_ID) {
        return FW_E_WRONG_BOARD;
    }
    uint32_t length = get_u32(&s->info[8]);
    uint32_t stored_crc = get_u32(&s->info[12]);
    if (length != s->next_offset || (~s->crc) != stored_crc) {
        return FW_E_BAD_CRC;
    }
    uint32_t profile = get_u32(&s->info[16]);
    if (out) {
        out->length = length;
        out->crc32 = stored_crc;
        out->profile = profile;
        memcpy(out->version, &s->info[20], 16);
        out->version[16] = '\0';
    }
    return (check_profile && profile != expected_profile) ? FW_E_WRONG_PROFILE : FW_OK;
}

fw_result_t fw_update_check(const char *name, uint32_t expected_profile, fw_file_info_t *info, fw_progress_fn progress)
{
    image_state_t s;
    memset(&s, 0, sizeof(s));
    s.crc = 0xFFFFFFFFUL;

    fw_result_t r = read_file(name, &s, "CHECKING", progress);
    if (r != FW_OK) {
        return r;
    }
    return judge_image(&s, expected_profile, true, info);
}

/* Reads an image back from a slot, the way the bootloader will */
static bool slot_info(uint32_t base, fw_file_info_t *out)
{
    const uint8_t *image = fw_hal_ptr(base);
    image_state_t s;
    memset(&s, 0, sizeof(s));
    memcpy(s.head, image, sizeof(s.head));
    uint32_t sp = get_u32(&s.head[0]);
    uint32_t reset = get_u32(&s.head[4]);
    uint32_t info_addr = get_u32(&s.head[FW_VECTOR_IMAGE_INFO * 4]);
    if (sp < 0x10000000UL || sp > 0x10008000UL || reset < FW_SLOT_A_BASE || reset >= (FW_SLOT_A_BASE + FW_SLOT_SIZE) ||
        info_addr < (FW_SLOT_A_BASE + sizeof(s.head)) || info_addr > (FW_SLOT_A_BASE + FW_SLOT_SIZE - INFO_SIZE)) {
        return false;
    }
    uint32_t info_off = info_addr - FW_SLOT_A_BASE;
    const uint8_t *info = image + info_off;
    uint32_t length = get_u32(info + 8);
    if (get_u32(info) != FW_IMAGE_MAGIC || get_u32(info + 4) != FW_BOARD_ID ||
        length < (info_off + INFO_SIZE) || length > FW_SLOT_SIZE) {
        return false;
    }
    s.crc = 0xFFFFFFFFUL;
    for (uint32_t i = 0; i < length; i++) {
        uint8_t b = image[i];
        if (i >= (info_off + INFO_CRC_OFFSET) && i < (info_off + INFO_CRC_OFFSET + 4)) {
            b = 0;
        }
        s.crc = crc_byte(s.crc, b);
    }
    if ((~s.crc) != get_u32(info + 12)) {
        return false;
    }
    if (out) {
        out->length = length;
        out->crc32 = get_u32(info + 12);
        out->profile = get_u32(info + 16);
        memcpy(out->version, info + 20, 16);
        out->version[16] = '\0';
    }
    return true;
}

bool fw_update_slot_b_info(fw_file_info_t *info)
{
    return slot_info(FW_SLOT_B_BASE, info);
}

fw_result_t fw_update_stage(const char *name, const fw_file_info_t *checked, fw_progress_fn progress)
{
    if (progress) {
        progress("ERASING", 0);
    }
    for (uint32_t i = 0; i < FW_SLOT_SECTORS; i++) {
        if (!fw_hal_erase(FW_SLOT_B_SECTOR + i)) {
            return FW_E_FLASH;
        }
    }

    image_state_t s;
    memset(&s, 0, sizeof(s));
    s.crc = 0xFFFFFFFFUL;
    s.writing = true;

    fw_result_t r = read_file(name, &s, "COPYING", progress);
    if (r != FW_OK) {
        return r;
    }
    if (!flush_page(&s)) {
        return FW_E_FLASH;
    }

    /* The file could have changed, so this copy is judged by itself, then read back from flash */
    fw_file_info_t written;
    r = judge_image(&s, 0, false, &written);
    if (r != FW_OK) {
        return r;
    }
    if (written.crc32 != checked->crc32 || written.length != checked->length) {
        return FW_E_BAD_CRC;
    }

    if (progress) {
        progress("VERIFYING", 0);
    }
    fw_file_info_t in_flash;
    if (!slot_info(FW_SLOT_B_BASE, &in_flash) || in_flash.crc32 != checked->crc32) {
        return FW_E_FLASH;
    }

    /* Only now can anything happen at the next boot */
    return fw_journal_append(FW_J_PENDING, FW_OP_NONE, 0) ? FW_OK : FW_E_JOURNAL;
}

const char *fw_update_error_text(fw_result_t r)
{
    switch (r) {
    case FW_E_NOT_FOUND:     return "FILE NOT FOUND";
    case FW_E_READ:          return "SD READ ERROR";
    case FW_E_BAD_LINE:      return "FILE DAMAGED";
    case FW_E_BAD_ADDRESS:   return "WRONG FILE TYPE";
    case FW_E_GAP:           return "FILE INCOMPLETE";
    case FW_E_NO_END:        return "FILE INCOMPLETE";
    case FW_E_NOT_AN_IMAGE:  return "NOT A FIRMWARE";
    case FW_E_WRONG_BOARD:   return "WRONG BOARD";
    case FW_E_WRONG_PROFILE: return "WRONG STN TYPE";
    case FW_E_BAD_CRC:       return "FILE DAMAGED";
    case FW_E_FLASH:         return "FLASH ERROR";
    case FW_E_JOURNAL:       return "FLASH ERROR";
    default:                 return "OK";
    }
}
