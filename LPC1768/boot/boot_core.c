/* What to do at power-up. Everything here is safe to cut at any instant: each step can be repeated,
 * and a step counts as done only once the journal says so.
 *
 * A swap moves one 32 KB sector at a time through the scratch sector:
 *   step 0: scratch <- A[i]     (A and B both intact)
 *   step 1: A[i]    <- B[i]     (the old A[i] is safe in scratch, B[i] intact)
 *   step 2: B[i]    <- scratch  (the new A[i] is in place, scratch intact)
 * Repeating any of these after a power cut gives the same result. Installing and rolling back are
 * the same swap. */
#include "boot_core.h"
#include "fw_journal.h"

#define SWAP_STEPS   (FW_SLOT_SECTORS * 3UL)
#define COPY_CHUNK   4096UL
#define COPY_TRIES   3

static uint32_t s_crc_table[256];
static bool s_crc_ready = false;
static union {
    uint8_t bytes[COPY_CHUNK];
    uint32_t align;
} s_copy_buf;

uint32_t fw_crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    if (!s_crc_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int b = 0; b < 8; b++) {
                c = (c >> 1) ^ (0xEDB88320UL & (0UL - (c & 1UL)));
            }
            s_crc_table[i] = c;
        }
        s_crc_ready = true;
    }
    for (uint32_t i = 0; i < len; i++) {
        crc = s_crc_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool fw_image_valid(uint32_t slot_base)
{
    const uint8_t *image = fw_hal_ptr(slot_base);

    /* Images are linked to run in slot A, wherever they sit now */
    uint32_t sp = read_u32(image);
    uint32_t reset = read_u32(image + 4);
    uint32_t info_addr = read_u32(image + FW_VECTOR_IMAGE_INFO * 4);
    if (sp < 0x10000000UL || sp > 0x10008000UL ||
        reset < FW_SLOT_A_BASE || reset >= (FW_SLOT_A_BASE + FW_SLOT_SIZE) ||
        info_addr < FW_SLOT_A_BASE || info_addr > (FW_SLOT_A_BASE + FW_SLOT_SIZE - sizeof(fw_image_info_t))) {
        return false;
    }
    uint32_t info_offset = info_addr - FW_SLOT_A_BASE;
    const uint8_t *info = image + info_offset;
    uint32_t length = read_u32(info + 8);
    uint32_t stored_crc = read_u32(info + 12);
    if (read_u32(info) != FW_IMAGE_MAGIC || read_u32(info + 4) != FW_BOARD_ID ||
        length < (info_offset + sizeof(fw_image_info_t)) || length > FW_SLOT_SIZE) {
        return false;
    }

    /* The CRC field itself counts as zero */
    static const uint8_t zeros[4] = {0, 0, 0, 0};
    uint32_t crc = 0xFFFFFFFFUL;
    crc = fw_crc32_update(crc, image, info_offset + 12);
    crc = fw_crc32_update(crc, zeros, 4);
    crc = fw_crc32_update(crc, info + 16, length - info_offset - 16);
    return (~crc) == stored_crc;
}

static bool bytes_equal(const uint8_t *a, const uint8_t *b, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

/* dst sector is erased, then filled from src and read back */
static bool copy_sector(uint32_t dst_sector, uint32_t dst_addr, uint32_t src_addr)
{
    for (int attempt = 0; attempt < COPY_TRIES; attempt++) {
        if (!fw_hal_erase(dst_sector)) {
            continue;
        }
        bool ok = true;
        for (uint32_t off = 0; off < FW_SECTOR_SIZE && ok; off += COPY_CHUNK) {
            const uint8_t *src = fw_hal_ptr(src_addr + off);
            for (uint32_t i = 0; i < COPY_CHUNK; i++) {
                s_copy_buf.bytes[i] = src[i];
            }
            ok = fw_hal_program(dst_addr + off, s_copy_buf.bytes, COPY_CHUNK) &&
                 bytes_equal(fw_hal_ptr(dst_addr + off), s_copy_buf.bytes, COPY_CHUNK);
        }
        if (ok) {
            return true;
        }
    }
    return false;
}

static bool run_swap(fw_op_t op, uint32_t first_step)
{
    for (uint32_t s = first_step; s < SWAP_STEPS; s++) {
        uint32_t i = s / 3;
        uint32_t a_addr = FW_SLOT_A_BASE + i * FW_SECTOR_SIZE;
        uint32_t b_addr = FW_SLOT_B_BASE + i * FW_SECTOR_SIZE;
        bool ok;
        switch (s % 3) {
        case 0:
            ok = copy_sector(FW_SCRATCH_SECTOR, FW_SCRATCH_BASE, a_addr);
            break;
        case 1:
            ok = copy_sector(FW_SLOT_A_SECTOR + i, a_addr, b_addr);
            break;
        default:
            ok = copy_sector(FW_SLOT_B_SECTOR + i, b_addr, FW_SCRATCH_BASE);
            break;
        }
        if (!ok || !fw_journal_append(FW_J_SWAP_STEP, op, (uint8_t)s)) {
            return false;
        }
    }
    return true;
}

/* A swap started or resumed: finish it, then record how it ended */
static bool finish_swap(fw_op_t op, uint32_t first_step)
{
    if (!run_swap(op, first_step)) {
        return false;
    }
    switch (op) {
    case FW_OP_INSTALL:
        return fw_journal_append(FW_J_SWAPPED, FW_OP_NONE, 0);
    case FW_OP_ROLLBACK:
        return fw_journal_append(FW_J_ROLLED_BACK, FW_OP_NONE, 0);
    case FW_OP_REVERT:
        return fw_journal_append(FW_J_REVERTED, FW_OP_NONE, 0);
    default:
        return fw_journal_append(FW_J_RECOVERED, FW_OP_NONE, 0);
    }
}

static bool start_swap(fw_op_t op)
{
    return fw_journal_append(FW_J_SWAP_START, op, 0) && finish_swap(op, 0);
}

/* Slot A on trial, with attempts boots so far */
static fw_boot_result_t run_trial(uint8_t attempts)
{
    if (attempts >= FW_TRIAL_MAX_BOOTS || !fw_image_valid(FW_SLOT_A_BASE)) {
        if (fw_image_valid(FW_SLOT_B_BASE)) {
            return start_swap(FW_OP_ROLLBACK) ? FW_BOOT_RUN : FW_BOOT_RETRY;
        }
        /* The old image is gone: the one on trial is all there is */
        if (fw_image_valid(FW_SLOT_A_BASE)) {
            return fw_journal_append(FW_J_CONFIRMED, FW_OP_NONE, 0) ? FW_BOOT_RUN : FW_BOOT_RETRY;
        }
        return FW_BOOT_NO_IMAGE;
    }
    if (!fw_journal_append(FW_J_TRIAL_BOOT, FW_OP_NONE, (uint8_t)(attempts + 1))) {
        return FW_BOOT_RETRY;
    }
    return FW_BOOT_RUN_TRIAL;
}

fw_boot_result_t fw_boot_decide(void)
{
    fw_record_t rec;
    fw_journal_read(&rec);

    switch (rec.type) {
    case FW_J_PENDING:
        if (!fw_image_valid(FW_SLOT_B_BASE)) {
            if (!fw_journal_append(FW_J_REJECTED, FW_OP_NONE, 0)) {
                return FW_BOOT_RETRY;
            }
            break;
        }
        if (!start_swap(FW_OP_INSTALL)) {
            return FW_BOOT_RETRY;
        }
        return run_trial(0);

    case FW_J_REVERT_REQ:
        if (!fw_image_valid(FW_SLOT_B_BASE)) {
            if (!fw_journal_append(FW_J_REJECTED, FW_OP_NONE, 0)) {
                return FW_BOOT_RETRY;
            }
            break;
        }
        return start_swap(FW_OP_REVERT) ? FW_BOOT_RUN : FW_BOOT_RETRY;

    case FW_J_SWAP_START:
    case FW_J_SWAP_STEP: {
        fw_op_t op = (fw_op_t)rec.op;
        uint32_t next = (rec.type == FW_J_SWAP_STEP) ? ((uint32_t)rec.arg + 1) : 0;
        if (!finish_swap(op, next)) {
            return FW_BOOT_RETRY;
        }
        return (op == FW_OP_INSTALL) ? run_trial(0) : FW_BOOT_RUN;
    }

    case FW_J_SWAPPED:
        return run_trial(0);

    case FW_J_TRIAL_BOOT:
        return run_trial(rec.arg);

    default:
        break;
    }

    /* Nothing in progress */
    if (fw_image_valid(FW_SLOT_A_BASE)) {
        return FW_BOOT_RUN;
    }
    if (fw_image_valid(FW_SLOT_B_BASE)) {
        return start_swap(FW_OP_RECOVER) ? FW_BOOT_RUN : FW_BOOT_RETRY;
    }
    return FW_BOOT_NO_IMAGE;
}
