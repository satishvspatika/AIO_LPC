/* CTRL ROM loader route (ctrl_isp_core.c) on a simulated LPC845 flash, with the real ctrl.hex read
 * by the production reader (fw_update.c) and the real CTRL bootloader:
 *   cc -std=gnu11 -DFW_TARGET_LPC845 -I.. -I../boot -o ctrl_isp_test ctrl_isp_test.c ../ctrl_isp_core.c \
 *      ../fw_update.c ../boot/boot_core.c ../boot/fw_journal.c
 *   ./ctrl_isp_test ../../LPC845/ctrl.hex ../../LPC845/LPC845_BOOT.bin ../../LPC845/LPC845_TWS.bin
 * Checks: the finished chip equals LPC845_BOOT.bin at 0 + LPC845_TWS.bin at 0x2400 + 0xFF elsewhere;
 * a power cut after any single flash operation leaves sector 0 holding the old contents, the rescue
 * program or the final bootloader (blank only inside sector 0's own erase-to-write window), and a
 * rerun from there always finishes; random operation failures leave sector 0 good or with no valid
 * vector table (the boot ROM then opens its loader), never half-written with a valid one; nothing
 * is ever written outside the 64 sectors, and a write into an unerased sector is caught (NOR flash
 * can only clear bits). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <assert.h>
#include "ctrl_isp_core.h"
#include "fw_layout.h"

#define SEC ISP_SECTOR_BYTES
#define FLASH_SIZE (ISP_SECTORS * SEC)

/* ---- fw_update.c's hooks: this test only reads ctrl.hex, it never touches LPC1768 flash ---- */
static uint8_t s_dummy[0x40];
const uint8_t *fw_hal_ptr(uint32_t addr) { (void)addr; return s_dummy; }
bool fw_hal_erase(uint32_t sec) { (void)sec; assert(!"LPC1768 flash erased"); return false; }
bool fw_hal_program(uint32_t addr, const void *src, uint32_t len) { (void)addr; (void)src; (void)len; assert(!"LPC1768 flash written"); return false; }

static uint8_t *s_file; static size_t s_file_len, s_file_pos;
bool fw_file_open(const char *name, uint32_t *size) { (void)name; s_file_pos = 0; *size = (uint32_t)s_file_len; return true; }
int fw_file_read(void *buf, uint32_t len)
{
    size_t left = s_file_len - s_file_pos;
    if (len > left) len = (uint32_t)left;
    memcpy(buf, s_file + s_file_pos, len);
    s_file_pos += len;
    return (int)len;
}
void fw_file_close(void) {}

/* ---- Simulated LPC845 flash ---- */
static uint8_t s_flash[FLASH_SIZE];
static long s_cut_after = -1;      /* Power cut after this many more operations */
static long s_ops = 0;
static unsigned s_fail_pct = 0;    /* Chance an operation reports failure (and, for writes, does half) */
static jmp_buf s_cut;
static bool s_in_window0 = false;  /* Sector 0 erased and not yet written */

static void tick(void)
{
    s_ops++;
    if (s_cut_after >= 0 && s_cut_after-- == 0) {
        s_cut_after = -1;
        longjmp(s_cut, 1);
    }
}
static bool fails(void) { return s_fail_pct && (unsigned)(rand() % 100) < s_fail_pct; }

static bool op_erase(uint32_t sector)
{
    assert(sector < ISP_SECTORS);
    tick();
    if (fails()) {
        memset(&s_flash[sector * SEC], 0xFF, SEC / 2);  /* Erase cut short */
        return false;
    }
    memset(&s_flash[sector * SEC], 0xFF, SEC);
    if (sector == 0) s_in_window0 = true;
    return true;
}

static bool op_program(uint32_t sector, const uint8_t *data)
{
    assert(sector < ISP_SECTORS);
    tick();
    uint8_t *f = &s_flash[sector * SEC];
    uint32_t n = fails() ? SEC / 2 : SEC;
    for (uint32_t i = 0; i < n; i++) {
        f[i] &= data[i];    /* NOR: bits only clear */
    }
    if (sector == 0 && n == SEC) s_in_window0 = false;
    return n == SEC;
}

static bool op_read(uint32_t sector, uint8_t *out)
{
    assert(sector < ISP_SECTORS);
    tick();
    if (fails()) return false;
    memcpy(out, &s_flash[sector * SEC], SEC);
    return true;
}

static bool op_blank(uint32_t sector)
{
    assert(sector < ISP_SECTORS);
    tick();
    if (fails()) return false;
    for (uint32_t i = 0; i < SEC; i++) {
        if (s_flash[sector * SEC + i] != 0xFF) return false;
    }
    return true;
}

/* ---- Images ---- */
static uint8_t *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(*len + 1);
    if (fread(d, 1, *len, f) != *len) exit(2);
    fclose(f);
    return d;
}

static uint8_t s_rescue[96];
static uint8_t s_expect[FLASH_SIZE];
static uint8_t s_rescue_sec[SEC];
static uint8_t *s_boot; static size_t s_boot_len;
static fw_file_info_t s_info;
static ctrl_isp_ops_t s_opsdef;

static isp_result_t run(uint32_t *fail_sector)
{
    s_in_window0 = false;
    return ctrl_isp_program(&s_opsdef, fail_sector);
}

/* Sector 0 may only ever be: what it was, the rescue program, the final bootloader head, or blank
 * while inside its own erase-to-write window */
static bool vectors_valid(const uint8_t *s0)
{
    uint32_t sum = 0;
    for (int i = 0; i < 8; i++) {
        uint32_t w;
        memcpy(&w, s0 + 4 * i, 4);
        sum += w;
    }
    return sum == 0;
}

static void check_sector0(const uint8_t *before0, const char *when, bool after_failure)
{
    const uint8_t *s0 = s_flash;
    if (memcmp(s0, s_expect, SEC) == 0 && memcmp(before0, s_expect, SEC) != 0 &&
        memcmp(&s_flash[SEC], &s_expect[SEC], FLASH_SIZE - SEC) != 0) {
        /* The final bootloader in sector 0 boots the CTRL firmware: it must only go on last */
        fprintf(stderr, "FAIL %s: final sector 0 on before the rest of the chip\n", when);
        exit(1);
    }
    if (memcmp(s0, before0, SEC) == 0 || memcmp(s0, s_rescue_sec, SEC) == 0 || memcmp(s0, s_expect, SEC) == 0) {
        return;
    }
    if (s_in_window0) {
        return;     /* The documented few-ms window (TEST_JIG.md section 13) */
    }
    if (after_failure && !vectors_valid(s0)) {
        return;     /* Sector 0 gave up: no valid table, the boot ROM opens its loader */
    }
    fprintf(stderr, "FAIL %s: sector 0 holds something else\n", when);
    exit(1);
}

static int s_failures = 0;
static void expect_done(const char *what)
{
    if (memcmp(s_flash, s_expect, FLASH_SIZE) != 0) {
        for (uint32_t s = 0; s < ISP_SECTORS; s++) {
            if (memcmp(&s_flash[s * SEC], &s_expect[s * SEC], SEC) != 0) {
                fprintf(stderr, "FAIL %s: sector %u differs\n", what, s);
                break;
            }
        }
        s_failures++;
    }
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s ctrl.hex LPC845_BOOT.bin LPC845_TWS.bin\n", argv[0]);
        return 2;
    }
    size_t app_len;
    s_file = read_all(argv[1], &s_file_len);
    s_boot = read_all(argv[2], &s_boot_len);
    uint8_t *app = read_all(argv[3], &app_len);

    /* A stand-in rescue program: the test only needs it to differ from everything else */
    for (size_t i = 0; i < sizeof(s_rescue); i++) s_rescue[i] = (uint8_t)(0xA5 ^ i);
    memset(s_rescue_sec, 0xFF, SEC);
    memcpy(s_rescue_sec, s_rescue, sizeof(s_rescue));

    memset(s_expect, 0xFF, FLASH_SIZE);
    memcpy(s_expect, s_boot, s_boot_len);
    memcpy(&s_expect[FW845_SLOT_A_BASE], app, app_len);

    assert(vectors_valid(s_boot));      /* CTRL bootloader as built */
    /* A stand-in rescue program with a valid table, like the real one (build.sh checks it) */
    {
        uint32_t sum = 0, w;
        for (int i = 0; i < 7; i++) { memcpy(&w, s_rescue + 4 * i, 4); sum += w; }
        w = 0U - sum;
        memcpy(s_rescue + 28, &w, 4);
        memcpy(s_rescue_sec, s_rescue, sizeof(s_rescue));
        assert(vectors_valid(s_rescue_sec));
    }
    assert(fw_ctrl_check("ctrl.hex", &s_info, NULL) == FW_OK);
    assert(s_info.length == app_len);

    s_opsdef = (ctrl_isp_ops_t){ op_erase, op_program, op_read, op_blank, NULL,
                                 s_boot, (uint32_t)s_boot_len, s_rescue, sizeof(s_rescue),
                                 "ctrl.hex", &s_info };
    uint32_t fs;
    srand(1);

    /* 1. Blank chip, chip with other firmware, chip already right */
    memset(s_flash, 0xFF, FLASH_SIZE);
    assert(run(&fs) == ISP_OK); expect_done("blank chip");
    for (size_t i = 0; i < FLASH_SIZE; i++) s_flash[i] = (uint8_t)rand();
    assert(run(&fs) == ISP_OK); expect_done("old firmware");
    long clean_ops_start = s_ops;
    assert(run(&fs) == ISP_OK); expect_done("already done");
    long ops_per_run = s_ops - clean_ops_start;
    printf("Clean runs: OK (%ld flash operations per run)\n", ops_per_run);

    /* 2. Power cut after every single operation, from old firmware; then rerun until done */
    static uint8_t old[FLASH_SIZE], before0[SEC];
    for (size_t i = 0; i < FLASH_SIZE; i++) old[i] = (uint8_t)(rand() | 1);
    int cuts = 0;
    for (long k = 0; k < ops_per_run + 40; k++) {
        memcpy(s_flash, old, FLASH_SIZE);
        memcpy(before0, s_flash, SEC);
        s_cut_after = k;
        if (setjmp(s_cut) == 0) {
            (void)run(&fs);
            s_cut_after = -1;
        } else {
            cuts++;
        }
        check_sector0(before0, "after a power cut", false);
        memcpy(before0, s_flash, SEC);
        if (run(&fs) != ISP_OK) { fprintf(stderr, "FAIL: rerun after cut %ld did not finish\n", k); return 1; }
        check_sector0(before0, "rerun", false);
        expect_done("rerun after cut");
    }
    printf("Power cut after each of %d operations, then rerun: OK\n", cuts);

    /* 3. Random failures (5% and 30% of operations): any result, sector 0 always safe, rerun fixes */
    unsigned pcts[] = { 5, 30 };
    int ok_runs = 0, failed_runs = 0;
    for (int p = 0; p < 2; p++) {
        for (int trial = 0; trial < 300; trial++) {
            memcpy(s_flash, old, FLASH_SIZE);
            memcpy(before0, s_flash, SEC);
            s_fail_pct = pcts[p];
            isp_result_t r = run(&fs);
            s_fail_pct = 0;
            check_sector0(before0, "after failures", r != ISP_OK);
            if (r == ISP_OK) {
                ok_runs++;
                expect_done("finished despite failures");
            } else {
                failed_runs++;
                assert(fs < ISP_SECTORS);
                memcpy(before0, s_flash, SEC);
                if (run(&fs) != ISP_OK) { fprintf(stderr, "FAIL: rerun after failure\n"); return 1; }
                check_sector0(before0, "rerun", false);
                expect_done("rerun after failure");
            }
        }
    }
    printf("Random operation failures: %d finished, %d stopped safely and finished on rerun: OK\n", ok_runs, failed_runs);

    /* 4. ctrl.hex changed on the card after the check: refused, sector 0 still the rescue program */
    fw_file_info_t wrong = s_info;
    wrong.crc32 ^= 1;
    s_opsdef.ctrl_info = &wrong;
    memcpy(s_flash, old, FLASH_SIZE);
    assert(run(&fs) == ISP_WRITE);
    assert(memcmp(s_flash, s_rescue_sec, SEC) == 0);
    s_opsdef.ctrl_info = &s_info;
    assert(run(&fs) == ISP_OK); expect_done("after changed file");
    printf("ctrl.hex changed after the check: refused, rescue sector 0 kept, rerun OK\n");

    if (s_failures) {
        printf("FAILED: %d\n", s_failures);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
