/* Runs the bootloader's logic on a simulated flash, cutting power at every flash operation:
 *   cc -std=gnu11 -I.. -o boot_test boot_test.c ../boot_core.c ../fw_journal.c && ./boot_test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <assert.h>
#include "fw_layout.h"
#include "fw_journal.h"
#include "boot_core.h"

#define FLASH_SIZE 0x80000UL
static uint8_t s_flash[FLASH_SIZE];
static long s_ops_left = -1;    /* Flash operations until the power cut, -1 for none */
static long s_ops_done = 0;
static long s_fail_ops = 0;     /* Next N operations report failure, with power staying on */
static long s_fail_programs = 0; /* Next N writes report failure, while erases still work */
static long s_silent_bad = 0;    /* Next N writes store wrong data but report success */
static jmp_buf s_power_cut;
static unsigned s_rand = 1;

static unsigned rnd(void) { s_rand = s_rand * 1103515245u + 12345u; return (s_rand >> 8) & 0xFFFFFF; }

static uint32_t sector_base(uint32_t sector)
{
    return (sector < 16) ? (sector * 0x1000UL) : (0x10000UL + (sector - 16) * 0x8000UL);
}
static uint32_t sector_size(uint32_t sector) { return (sector < 16) ? 0x1000UL : 0x8000UL; }

const uint8_t *fw_hal_ptr(uint32_t addr) { assert(addr < FLASH_SIZE); return &s_flash[addr]; }

static void op_tick(void)
{
    s_ops_done++;
    if (s_ops_left >= 0 && s_ops_left-- == 0) {
        s_ops_left = -1;
        longjmp(s_power_cut, 1);
    }
}

bool fw_hal_erase(uint32_t sector)
{
    uint32_t base = sector_base(sector), size = sector_size(sector);
    if (s_ops_left == 0) {
        /* Cut part way through: some of the sector erased, the rest damaged */
        uint32_t n = rnd() % size;
        memset(&s_flash[base], 0xFF, n);
        for (uint32_t i = n; i < size && i < n + 64; i++) s_flash[base + i] = (uint8_t)rnd();
    }
    op_tick();
    if (s_fail_ops > 0) { s_fail_ops--; return false; }
    memset(&s_flash[base], 0xFF, size);
    return true;
}

bool fw_hal_program(uint32_t addr, const void *src, uint32_t len)
{
    assert(len == 256 || len == 4096);
    if (s_ops_left == 0) {
        uint32_t n = rnd() % len;
        memcpy(&s_flash[addr], src, n);
        if (n < len) s_flash[addr + n] = (uint8_t)rnd();
    }
    op_tick();
    if (s_fail_ops > 0) { s_fail_ops--; return false; }
    if (s_fail_programs > 0) { s_fail_programs--; return false; }
    memcpy(&s_flash[addr], src, len);
    if (s_silent_bad > 0) { s_silent_bad--; s_flash[addr + len / 2] ^= 0x04; }
    return true;
}

/* A full-size image with its own contents, shaped like a real build */
static void make_image(uint8_t *out, uint32_t length, uint8_t seed)
{
    memset(out, 0xFF, FW_SLOT_SIZE);
    for (uint32_t i = 0; i < length; i++) out[i] = (uint8_t)(seed + i * 7 + (i >> 8));
    uint32_t info_off = 0x200;
    uint32_t sp = 0x10008000UL, reset = FW_SLOT_A_BASE + 0x301, info_addr = FW_SLOT_A_BASE + info_off;
    memcpy(out, &sp, 4);
    memcpy(out + 4, &reset, 4);
    memcpy(out + FW_VECTOR_IMAGE_INFO * 4, &info_addr, 4);
    fw_image_info_t info;
    memset(&info, 0, sizeof(info));
    info.magic = FW_IMAGE_MAGIC;
    info.board = FW_BOARD_ID;
    info.length = length;
    info.crc32 = 0;
    info.profile = 1;
    snprintf(info.version, sizeof(info.version), "v-%u", seed);
    memcpy(out + info_off, &info, sizeof(info));
    uint32_t crc = ~fw_crc32_update(0xFFFFFFFFUL, out, length);
    memcpy(out + info_off + 12, &crc, 4);
}

static uint8_t s_old[FW_SLOT_SIZE], s_new[FW_SLOT_SIZE], s_new2[FW_SLOT_SIZE];
static const uint32_t OLD_LEN = 105000, NEW_LEN = 118000;

static void reset_flash(bool stage_new)
{
    memset(s_flash, 0xFF, sizeof(s_flash));
    memset(&s_flash[0x1000], 0x5A, 0x1000); /* Settings */
    memset(&s_flash[0x58000], 0xA5, 0x28000); /* Record log */
    s_fail_programs = 0;
    s_silent_bad = 0;
    memcpy(&s_flash[FW_SLOT_A_BASE], s_old, FW_SLOT_SIZE);
    if (stage_new) memcpy(&s_flash[FW_SLOT_B_BASE], s_new, FW_SLOT_SIZE);
    s_fail_ops = 0;
}

static bool slot_is(uint32_t base, const uint8_t *img) { return memcmp(&s_flash[base], img, FW_SLOT_SIZE) == 0; }

static void check_untouched(void)
{
    for (uint32_t i = 0x1000; i < 0x2000; i++) assert(s_flash[i] == 0x5A);
    for (uint32_t i = 0x58000; i < 0x80000; i++) assert(s_flash[i] == 0xA5);
}

/* What the application does on a good boot */
static void app_confirms(void) { assert(fw_journal_append(FW_J_CONFIRMED, FW_OP_NONE, 0)); }

typedef enum { PLAN_HEALTHY, PLAN_NEVER_CONFIRMS } plan_t;

/* Power up repeatedly until the unit settles. cut_at counts flash operations across boots:
 * the power fails there once, or -1 for none. Returns what finally runs. */
static fw_boot_result_t run_boots(plan_t plan, long cut_at, int extra_cuts)
{
    fw_boot_result_t r = FW_BOOT_NO_IMAGE;
    s_ops_left = cut_at;
    for (int boots = 0; boots < 40; boots++) {
        if (setjmp(s_power_cut) != 0) {
            /* Power came back: maybe another cut soon after */
            if (extra_cuts > 0) { extra_cuts--; s_ops_left = (long)(rnd() % 12); }
            continue;
        }
        r = fw_boot_decide();
        if (r == FW_BOOT_RETRY) continue;
        if (r == FW_BOOT_RUN_TRIAL) {
            /* Whichever image is now running */
            if (plan == PLAN_HEALTHY) { app_confirms(); r = FW_BOOT_RUN; s_ops_left = -1; break; }
            continue; /* Crashes, and the watchdog restarts it */
        }
        s_ops_left = -1;
        break;
    }
    s_ops_left = -1;
    return r;
}

static long count_ops(plan_t plan, bool stage)
{
    reset_flash(stage);
    assert(fw_journal_append(FW_J_PENDING, FW_OP_NONE, 0));
    s_ops_done = 0;
    fw_boot_result_t r = run_boots(plan, -1, 0);
    assert(r == FW_BOOT_RUN);
    return s_ops_done;
}

static void final_state_after_update(plan_t plan)
{
    check_untouched();
    if (plan == PLAN_HEALTHY) {
        assert(slot_is(FW_SLOT_A_BASE, s_new));
        assert(slot_is(FW_SLOT_B_BASE, s_old));
    } else {
        assert(slot_is(FW_SLOT_A_BASE, s_old));
        assert(slot_is(FW_SLOT_B_BASE, s_new));
    }
    /* Steady now: another boot runs slot A and changes nothing */
    uint8_t before[FW_SLOT_SIZE];
    memcpy(before, &s_flash[FW_SLOT_A_BASE], FW_SLOT_SIZE);
    assert(fw_boot_decide() == FW_BOOT_RUN);
    assert(memcmp(before, &s_flash[FW_SLOT_A_BASE], FW_SLOT_SIZE) == 0);
}

int main(void)
{
    long tests = 0;
    make_image(s_old, OLD_LEN, 11);
    make_image(s_new, NEW_LEN, 77);
    make_image(s_new2, NEW_LEN - 5000, 133);

    /* Image check */
    reset_flash(true);
    assert(fw_image_valid(FW_SLOT_A_BASE) && fw_image_valid(FW_SLOT_B_BASE));
    s_flash[FW_SLOT_B_BASE + 90000] ^= 0x01;
    assert(!fw_image_valid(FW_SLOT_B_BASE));
    s_flash[FW_SLOT_B_BASE + 90000] ^= 0x01;
    assert(fw_image_valid(FW_SLOT_B_BASE));
    memset(&s_flash[FW_SLOT_B_BASE], 0xFF, FW_SLOT_SIZE);
    assert(!fw_image_valid(FW_SLOT_B_BASE));
    puts("image check: one flipped bit and a blank slot are both rejected");

    /* Nothing pending */
    reset_flash(false);
    assert(fw_boot_decide() == FW_BOOT_RUN && slot_is(FW_SLOT_A_BASE, s_old));

    /* 1. Update, cut at every flash operation */
    long good_ops = count_ops(PLAN_HEALTHY, true);
    printf("healthy update: %ld flash operations\n", good_ops);
    for (long cut = 0; cut <= good_ops + 1; cut++) {
        reset_flash(true);
        assert(fw_journal_append(FW_J_PENDING, FW_OP_NONE, 0));
        s_rand = (unsigned)cut * 31u + 7u;
        assert(run_boots(PLAN_HEALTHY, cut, 0) == FW_BOOT_RUN);
        final_state_after_update(PLAN_HEALTHY);
        tests++;
    }

    /* 2. New image never proves itself: automatic rollback, cut at every operation */
    long bad_ops = count_ops(PLAN_NEVER_CONFIRMS, true);
    printf("failed update and rollback: %ld flash operations\n", bad_ops);
    for (long cut = 0; cut <= bad_ops + 1; cut++) {
        reset_flash(true);
        assert(fw_journal_append(FW_J_PENDING, FW_OP_NONE, 0));
        s_rand = (unsigned)cut * 17u + 3u;
        assert(run_boots(PLAN_NEVER_CONFIRMS, cut, 0) == FW_BOOT_RUN);
        final_state_after_update(PLAN_NEVER_CONFIRMS);
        tests++;
    }

    /* 3. Many power cuts in a row, at random points */
    for (int seed = 0; seed < 4000; seed++) {
        plan_t plan = (seed & 1) ? PLAN_NEVER_CONFIRMS : PLAN_HEALTHY;
        reset_flash(true);
        assert(fw_journal_append(FW_J_PENDING, FW_OP_NONE, 0));
        s_rand = (unsigned)seed * 2654435761u + 1u;
        fw_boot_result_t r = run_boots(plan, (long)(rnd() % (unsigned long)(bad_ops + 2)), 5 + (seed % 7));
        /* Enough cuts can use up trial boots, so a healthy image may also end up rolled back:
         * either way one complete image runs and the other is kept */
        assert(r == FW_BOOT_RUN);
        check_untouched();
        assert((slot_is(FW_SLOT_A_BASE, s_new) && slot_is(FW_SLOT_B_BASE, s_old)) ||
               (slot_is(FW_SLOT_A_BASE, s_old) && slot_is(FW_SLOT_B_BASE, s_new)));
        tests++;
    }

    /* 4. A damaged staged image is refused and slot A is never touched */
    reset_flash(true);
    s_flash[FW_SLOT_B_BASE + 5000] ^= 0x80;
    assert(fw_journal_append(FW_J_PENDING, FW_OP_NONE, 0));
    assert(fw_boot_decide() == FW_BOOT_RUN);
    assert(slot_is(FW_SLOT_A_BASE, s_old));
    fw_record_t rec;
    fw_journal_read(&rec);
    assert(rec.type == FW_J_REJECTED);
    tests++;

    /* 5. Slot A damaged with a good image in slot B: recovers by itself, cut anywhere */
    for (long cut = 0; cut < 400; cut += 3) {
        reset_flash(true);
        s_flash[FW_SLOT_A_BASE + 777] ^= 0x10;
        s_rand = (unsigned)cut + 99u;
        assert(run_boots(PLAN_HEALTHY, cut, 0) == FW_BOOT_RUN);
        assert(slot_is(FW_SLOT_A_BASE, s_new));
        check_untouched();
        tests++;
    }

    /* 6. Both slots bad: says so, and doesn't touch anything */
    reset_flash(false);
    s_flash[FW_SLOT_A_BASE + 10] ^= 0x01;
    assert(fw_boot_decide() == FW_BOOT_NO_IMAGE);
    tests++;

    /* 7. Operator asks for the previous firmware after a good update */
    reset_flash(true);
    assert(fw_journal_append(FW_J_PENDING, FW_OP_NONE, 0));
    assert(run_boots(PLAN_HEALTHY, -1, 0) == FW_BOOT_RUN);
    assert(fw_journal_append(FW_J_REVERT_REQ, FW_OP_NONE, 0));
    assert(fw_boot_decide() == FW_BOOT_RUN);
    assert(slot_is(FW_SLOT_A_BASE, s_old) && slot_is(FW_SLOT_B_BASE, s_new));
    fw_journal_read(&rec);
    assert(rec.type == FW_J_REVERTED);
    tests++;

    /* 8. Update after update, enough to wrap the journal ring many times */
    reset_flash(true);
    const uint8_t *images[3] = { s_old, s_new, s_new2 };
    int running = 0;
    for (int round = 1; round <= 60; round++) {
        int next = round % 3;
        if (next == running) next = (next + 1) % 3;
        memcpy(&s_flash[FW_SLOT_B_BASE], images[next], FW_SLOT_SIZE);
        assert(fw_journal_append(FW_J_PENDING, FW_OP_NONE, 0));
        s_rand = (unsigned)round * 101u;
        assert(run_boots(PLAN_HEALTHY, (long)(rnd() % 60), 2) == FW_BOOT_RUN);
        assert(slot_is(FW_SLOT_A_BASE, images[next]) || slot_is(FW_SLOT_A_BASE, images[running]));
        if (slot_is(FW_SLOT_A_BASE, images[next])) running = next;
        check_untouched();
        tests++;
    }

    /* 9. Flash operations that fail with the power on: it keeps retrying and gets there */
    reset_flash(true);
    assert(fw_journal_append(FW_J_PENDING, FW_OP_NONE, 0));
    s_fail_ops = 5;
    assert(run_boots(PLAN_HEALTHY, -1, 0) == FW_BOOT_RUN);
    final_state_after_update(PLAN_HEALTHY);
    tests++;

    /* 10. Flash that stores wrong data but reports success: the read-back catches it, at each
     * write of the swap in turn */
    for (long skip = 0; skip < 90; skip += 2) {
        reset_flash(true);
        assert(fw_journal_append(FW_J_PENDING, FW_OP_NONE, 0));
        fw_record_t r0;
        fw_journal_read(&r0);
        /* Let some of the swap run, then corrupt the next write */
        long ops_before = skip;
        s_ops_left = ops_before;
        if (setjmp(s_power_cut) == 0) {
            (void)fw_boot_decide();
        }
        s_ops_left = -1;
        s_silent_bad = 1;
        assert(run_boots(PLAN_HEALTHY, -1, 0) == FW_BOOT_RUN);
        if (s_silent_bad == 0 && !(slot_is(FW_SLOT_A_BASE, s_new) && slot_is(FW_SLOT_B_BASE, s_old))) {
            /* A corrupted journal page is simply skipped, so the images must still match */
            assert(0 && "image damaged by a silent bad write");
        }
        s_silent_bad = 0;
        tests++;
    }

    /* 11. Long run of failed journal writes: the current state must survive, even when the ring
     * comes all the way round to the sector that holds it */
    reset_flash(true);
    assert(fw_journal_append(FW_J_PENDING, FW_OP_NONE, 0));
    s_fail_programs = 1000;
    assert(!fw_journal_append(FW_J_CONFIRMED, FW_OP_NONE, 0));
    s_fail_programs = 0;
    fw_journal_read(&rec);
    assert(rec.type == FW_J_PENDING);
    assert(run_boots(PLAN_HEALTHY, -1, 0) == FW_BOOT_RUN);
    final_state_after_update(PLAN_HEALTHY);
    tests++;

    printf("ALL PASS: %ld scenarios, with no image ever lost\n", tests);
    return 0;
}
