#include "fw845_recv.h"
#include "fw_layout.h"
#include "fw_journal.h"
#include "boot_core.h"
#include <stdio.h>
#include <string.h>

static const char *s_version = "";
static uint32_t s_running_crc = 0;
static bool s_on_trial = false;
static bool s_active = false;   /* A transfer is going on */
static bool s_staged = false;   /* The whole image arrived and passed, ready to install */
static uint32_t s_length = 0;
static uint32_t s_crc = 0;
static uint32_t s_next = 0;
static union {
    uint8_t bytes[FW_SECTOR_SIZE];
    uint32_t align;
} s_buf;

void fw845_recv_init(const char *running_version, uint32_t running_crc)
{
    fw_record_t rec;
    s_version = running_version;
    s_running_crc = running_crc;
    s_active = false;
    s_staged = false;
    fw_journal_read(&rec);
    s_on_trial = (rec.type == FW_J_TRIAL_BOOT);
}

bool fw845_on_trial(void)
{
    return s_on_trial;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* n hex digits. False if any isn't one. */
static bool parse_hex(const char *p, int n, uint32_t *out)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        int d = hex_digit(p[i]);
        if (d < 0) {
            return false;
        }
        v = (v << 4) | (uint32_t)d;
    }
    *out = v;
    return true;
}

/* The frame's last field is the XOR of everything between '$' and the comma before it. Returns where
 * that comma is, or NULL for a damaged line. */
static const char *checked_frame_end(const char *cmd)
{
    const char *end = strchr(cmd, ';');
    uint32_t want;
    if (!end || (end - cmd) < 4 || end[-3] != ',' || !parse_hex(end - 2, 2, &want)) {
        return NULL;
    }
    uint8_t x = 0;
    for (const char *q = cmd + 1; q < end - 3; q++) {
        x ^= (uint8_t)*q;
    }
    return (x == (uint8_t)want) ? (end - 3) : NULL;
}

static bool write_sector(uint32_t offset)
{
    uint32_t addr = FW_SLOT_B_BASE + offset;
    if (!fw_hal_program(addr, s_buf.bytes, FW_SECTOR_SIZE)) {
        return false;
    }
    return memcmp(fw_hal_ptr(addr), s_buf.bytes, FW_SECTOR_SIZE) == 0;
}

/* $UB<length>,<crc>,<xor>; Clears staging. */
static fw845_err_t cmd_begin(const char *cmd)
{
    uint32_t length, crc;
    const char *end = checked_frame_end(cmd);
    if (!end) {
        return FW845_E_CHECKSUM;
    }
    if ((end - cmd) != 20 || cmd[11] != ',' || !parse_hex(cmd + 3, 8, &length) || !parse_hex(cmd + 12, 8, &crc)) {
        return FW845_E_FORMAT;
    }
    if (s_on_trial) {
        return FW845_E_ON_TRIAL;
    }
    if (length == 0 || length > FW_SLOT_SIZE) {
        return FW845_E_SIZE;
    }
    s_active = false;
    s_staged = false;
    for (uint32_t i = 0; i < FW_SLOT_SECTORS; i++) {
        if (!fw_hal_erase(FW_SLOT_B_SECTOR + i)) {
            return FW845_E_FLASH;
        }
    }
    s_length = length;
    s_crc = crc;
    s_next = 0;
    s_active = true;
    return FW845_OK;
}

/* $UD<offset>,<data>,<xor>; A chunk seen before is a reply that got lost: it's answered, not stored. */
static fw845_err_t cmd_data(const char *cmd)
{
    uint32_t offset, value;
    const char *end = checked_frame_end(cmd);
    if (!end) {
        return FW845_E_CHECKSUM;
    }
    if (!s_active) {
        return FW845_E_NOT_STARTED;
    }
    int digits = (int)(end - (cmd + 12));
    int count = digits / 2;
    if (cmd[11] != ',' || digits <= 0 || (digits & 1) || count > FW845_MAX_CHUNK || !parse_hex(cmd + 3, 8, &offset)) {
        return FW845_E_FORMAT;
    }
    if (offset < s_next) {
        return FW845_OK;
    }
    if (offset > s_next) {
        return FW845_E_OFFSET;
    }
    if (offset + (uint32_t)count > s_length) {
        return FW845_E_SIZE;
    }
    /* Check the digits first, so a bad one can't leave half a chunk behind */
    for (int i = 0; i < digits; i++) {
        if (hex_digit(cmd[12 + i]) < 0) {
            return FW845_E_FORMAT;
        }
    }
    for (int i = 0; i < count; i++) {
        parse_hex(cmd + 12 + i * 2, 2, &value);
        s_buf.bytes[s_next % FW_SECTOR_SIZE] = (uint8_t)value;
        s_next++;
        if ((s_next % FW_SECTOR_SIZE) == 0 && !write_sector(s_next - FW_SECTOR_SIZE)) {
            s_active = false;
            return FW845_E_FLASH;
        }
    }
    return FW845_OK;
}

static uint32_t read_flash_u32(uint32_t addr)
{
    const uint8_t *p = fw_hal_ptr(addr);
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* $UE; All of it has arrived: prove it, then mark it to install. */
static fw845_err_t cmd_end(void)
{
    if (s_staged) {
        return FW845_OK; /* The reply got lost */
    }
    if (!s_active || s_next != s_length) {
        return FW845_E_NOT_STARTED;
    }
    s_active = false;
    uint32_t tail = s_next % FW_SECTOR_SIZE;
    if (tail != 0) {
        memset(&s_buf.bytes[tail], 0xFF, FW_SECTOR_SIZE - tail);
        if (!write_sector(s_next - tail)) {
            return FW845_E_FLASH;
        }
    }
    if (!fw_image_valid(FW_SLOT_B_BASE)) {
        return FW845_E_IMAGE;
    }
    uint32_t info = FW_SLOT_B_BASE + (read_flash_u32(FW_SLOT_B_BASE + FW_VECTOR_IMAGE_INFO * 4) - FW_SLOT_A_BASE);
    if (read_flash_u32(info + 8) != s_length || read_flash_u32(info + 12) != s_crc) {
        return FW845_E_IMAGE;
    }
    if (!fw_journal_append(FW_J_PENDING, FW_OP_NONE, 0)) {
        return FW845_E_JOURNAL;
    }
    s_staged = true;
    return FW845_OK;
}

/* $Uq,<journal state>,<running crc>,<running version>; */
static void reply_state(char *reply, size_t reply_size)
{
    fw_record_t rec;
    fw_journal_read(&rec);
    snprintf(reply, reply_size, "$Uq,%u,%08lX,%.16s;\n", (unsigned)rec.type, (unsigned long)s_running_crc, s_version);
}

void fw845_recv_command(const char *cmd, char *reply, size_t reply_size, bool *reset_now)
{
    *reset_now = false;
    switch (cmd[2]) {
    case 'Q':
        reply_state(reply, reply_size);
        break;
    case 'B':
        snprintf(reply, reply_size, "$Ub,%d;\n", (int)cmd_begin(cmd));
        break;
    case 'D': {
        fw845_err_t e = cmd_data(cmd);
        snprintf(reply, reply_size, "$Ua,%08lX,%d;\n", (unsigned long)s_next, (int)e);
        break;
    }
    case 'E':
        snprintf(reply, reply_size, "$Ue,%d;\n", (int)cmd_end());
        break;
    case 'R':
        /* Only straight after a good transfer, so a stray command can't wipe the rain records in RAM */
        if (s_staged) {
            snprintf(reply, reply_size, "$Ur,0;\n");
            *reset_now = true;
        } else {
            snprintf(reply, reply_size, "$Ur,%d;\n", (int)FW845_E_NOT_STARTED);
        }
        break;
    case 'C':
        /* The LPC1768 has heard good frames from this firmware */
        if (s_on_trial && fw_journal_append(FW_J_CONFIRMED, FW_OP_NONE, 0)) {
            s_on_trial = false;
        }
        reply_state(reply, reply_size);
        break;
    case 'K': {
        /* The LPC1768 has logged how the last update ended */
        fw_record_t rec;
        fw_journal_read(&rec);
        if (rec.type == FW_J_CONFIRMED || rec.type == FW_J_ROLLED_BACK || rec.type == FW_J_RECOVERED || rec.type == FW_J_REJECTED) {
            fw_journal_append(FW_J_REPORTED, FW_OP_NONE, 0);
        }
        reply_state(reply, reply_size);
        break;
    }
    default:
        snprintf(reply, reply_size, "$Ux,%d;\n", (int)FW845_E_FORMAT);
        break;
    }
}
