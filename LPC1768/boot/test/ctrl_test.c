/* A whole LPC845 update on a simulated chip: the LPC1768's sender (fw_update.c, fw_ctrl.c) talking to
 * the LPC845's receiver (fw845_recv.c) and bootloader logic (boot_core.c), with a real ctrl.hex:
 *   cc -std=gnu11 -DFW_TARGET_LPC845 -I.. -I../.. -I../../../LPC845/boot -o ctrl_test ctrl_test.c \
 *      ../../fw_update.c ../../fw_ctrl.c ../../../LPC845/boot/fw845_recv.c ../boot_core.c ../fw_journal.c
 *   ./ctrl_test ../../../LPC845/ctrl.hex */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <assert.h>
#include "fw_ctrl.h"
#include "fw_layout.h"
#include "fw_journal.h"
#include "boot_core.h"
#include "fw845_recv.h"

/* ---- Flash ---- */
static uint8_t s_flash[0x10000];
static long s_ops_left = -1;
static long s_ops_done = 0;
static jmp_buf s_cut;
const uint8_t *fw_hal_ptr(uint32_t addr) { assert(addr < sizeof(s_flash)); return &s_flash[addr]; }
static void tick(void) { s_ops_done++; if (s_ops_left >= 0 && s_ops_left-- == 0) { s_ops_left = -1; longjmp(s_cut, 1); } }
bool fw_hal_erase(uint32_t sec)
{
    assert(sec >= 4 && sec < 57); /* Never the bootloader or anything above staging */
    tick();
    memset(&s_flash[sec * 0x400], 0xFF, 0x400);
    return true;
}
static bool s_in_boot = false;
bool fw_hal_program(uint32_t addr, const void *src, uint32_t len)
{
    assert(len == 64 || len == 1024);
    assert((addr % len) == 0);
    /* While it's running, the firmware only ever writes staging or the journal */
    if (!s_in_boot) {
        assert((addr >= FW_JOURNAL_BASE && addr < FW_JOURNAL_BASE + 0x1000) || (addr >= FW_SLOT_B_BASE && addr + len <= FW_SLOT_B_BASE + FW_SLOT_SIZE));
    }
    tick();
    memcpy(&s_flash[addr], src, len);
    return true;
}

/* ---- SD card ---- */
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

/* ---- The link between the chips, with faults ---- */
static char s_reply[64];
static bool s_reply_ready = false;
static bool s_reset_requested = false;
static long s_frames = 0;
static int s_lose_cmd_every = 0, s_lose_reply_every = 0, s_damage_every = 0, s_dead_after = -1;
static uint32_t s_old_crc = 0x11111111;

void fw_ctrl_tx(const char *frame)
{
    char copy[140];
    s_frames++;
    if (s_dead_after >= 0 && s_frames > s_dead_after) return;
    if (s_lose_cmd_every && (s_frames % s_lose_cmd_every) == 0) return;
    assert(strlen(frame) < sizeof(copy));
    assert(strlen(frame) <= 119); /* The LPC845's receive buffer drops anything longer */
    strcpy(copy, frame);
    if (s_damage_every && (s_frames % s_damage_every) == 0 && strlen(copy) > 20) {
        copy[12 + (s_frames % 8)] ^= 0x01;
    }
    bool reset_now = false;
    char reply[64];
    fw845_recv_command(copy, reply, sizeof(reply), &reset_now);
    if (reset_now) s_reset_requested = true;
    if (s_lose_reply_every && (s_frames % s_lose_reply_every) == 0) return;
    strcpy(s_reply, reply);
    s_reply_ready = true;
}

bool fw_ctrl_wait_reply(char kind, char *reply, size_t size, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (s_reply_ready && s_reply[2] == kind) {
        strncpy(reply, s_reply, size - 1);
        reply[size - 1] = '\0';
        s_reply_ready = false;
        return true;
    }
    s_reply_ready = false;
    return false;
}

/* ---- Images ---- */
static uint8_t s_new_img[FW_SLOT_SIZE], s_old_img[FW_SLOT_SIZE];
static uint32_t s_new_len;

static int hexv(char c) { return (c <= '9') ? c - '0' : c - 'A' + 10; }
static void load_image_from_hex(void)
{
    memset(s_new_img, 0xFF, sizeof(s_new_img));
    uint32_t base = 0;
    size_t i = 0;
    while (i < s_file_len) {
        char *p = (char *)s_file + i;
        char *nl = memchr(p, '\n', s_file_len - i);
        size_t ll = nl ? (size_t)(nl - p) : s_file_len - i;
        if (p[0] == ':') {
            int n = hexv(p[1]) * 16 + hexv(p[2]);
            int a = hexv(p[3]) * 4096 + hexv(p[4]) * 256 + hexv(p[5]) * 16 + hexv(p[6]);
            int t = hexv(p[7]) * 16 + hexv(p[8]);
            if (t == 2) base = (uint32_t)(hexv(p[9]) * 4096 + hexv(p[10]) * 256 + hexv(p[11]) * 16 + hexv(p[12])) << 4;
            if (t == 4) base = (uint32_t)(hexv(p[9]) * 4096 + hexv(p[10]) * 256 + hexv(p[11]) * 16 + hexv(p[12])) << 16;
            if (t == 0) for (int k = 0; k < n; k++) {
                uint32_t addr = base + (uint32_t)a + (uint32_t)k - FW_SLOT_A_BASE;
                s_new_img[addr] = (uint8_t)(hexv(p[9 + k * 2]) * 16 + hexv(p[10 + k * 2]));
                if (addr + 1 > s_new_len) s_new_len = addr + 1;
            }
        }
        i += ll + 1;
    }
}

/* The firmware running before the update: the same image, marked as a different build */
static void make_old_image(void)
{
    memcpy(s_old_img, s_new_img, sizeof(s_old_img));
    memcpy(&s_old_img[0xC0 + 20], "845-old", 8);
    memset(&s_old_img[0xC0 + 12], 0, 4);
    uint32_t crc = ~fw_crc32_update(0xFFFFFFFFUL, s_old_img, s_new_len);
    memcpy(&s_old_img[0xC0 + 12], &crc, 4);
    s_old_crc = crc;
}

static void fresh_unit(void)
{
    memset(s_flash, 0xFF, sizeof(s_flash));
    memset(&s_flash[0], 0x5A, 0x1000); /* Bootloader */
    memcpy(&s_flash[FW_SLOT_A_BASE], s_old_img, FW_SLOT_SIZE);
    s_frames = 0;
    s_lose_cmd_every = s_lose_reply_every = s_damage_every = 0;
    s_dead_after = -1;
    s_reset_requested = false;
    s_reply_ready = false;
    fw845_recv_init("845-old", s_old_crc);
}

static bool a_is(const uint8_t *img) { return memcmp(&s_flash[FW_SLOT_A_BASE], img, FW_SLOT_SIZE) == 0; }
static bool b_is(const uint8_t *img) { return memcmp(&s_flash[FW_SLOT_B_BASE], img, FW_SLOT_SIZE) == 0; }
static void boot_safe(void) { for (int i = 0; i < 0x1000; i++) assert(s_flash[i] == 0x5A); }

static fw_boot_result_t boot(void)
{
    s_in_boot = true;
    fw_boot_result_t r = fw_boot_decide();
    s_in_boot = false;
    return r;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *f = fopen(argv[1], "rb");
    assert(f);
    fseek(f, 0, SEEK_END); s_file_len = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    s_file = malloc(s_file_len + 1);
    assert(fread(s_file, 1, s_file_len, f) == s_file_len);
    fclose(f);
    load_image_from_hex();
    make_old_image();
    assert(s_new_len > 0x1000);

    fw_file_info_t info;
    fw_ctrl_state_t st;
    fw_record_t rec;

    /* The file checks out as the LPC845's */
    assert(fw_ctrl_check("sensor.hex", &info, NULL) == FW_OK);
    assert(info.length == s_new_len);
    printf("file: %s, %lu bytes\n", info.version, (unsigned long)info.length);

    /* 1. A clean update, then a healthy trial */
    fresh_unit();
    assert(fw_ctrl_query(&st) && st.state == FW_J_NONE && st.crc32 == s_old_crc && strcmp(st.version, "845-old") == 0);
    assert(fw_ctrl_send_image("sensor.hex", &info, NULL) == FW_OK);
    long clean_frames = s_frames;
    assert(a_is(s_old_img) && b_is(s_new_img));
    fw_journal_read(&rec); assert(rec.type == FW_J_PENDING);
    fw_ctrl_restart_lpc845(); assert(s_reset_requested);
    assert(boot() == FW_BOOT_RUN_TRIAL);
    assert(a_is(s_new_img) && b_is(s_old_img));
    fw845_recv_init(info.version, info.crc32);
    assert(fw845_on_trial());
    /* While on trial, staging is what it falls back to */
    { char frame[48] = "$UB00001000,00000000";
      uint8_t x = 0; for (size_t i = 1; i < strlen(frame); i++) x ^= (uint8_t)frame[i];
      snprintf(frame + strlen(frame), 8, ",%02X;", x);
      bool rn; char r[64]; fw845_recv_command(frame, r, sizeof(r), &rn); assert(strcmp(r, "$Ub,9;\n") == 0); assert(b_is(s_old_img)); }
    fw_ctrl_tx("$UC;\n");
    assert(!fw845_on_trial());
    fw_journal_read(&rec); assert(rec.type == FW_J_CONFIRMED);
    assert(fw_ctrl_query(&st) && st.crc32 == info.crc32);
    fw_ctrl_tx("$UK;\n");
    fw_journal_read(&rec); assert(rec.type == FW_J_REPORTED);
    assert(boot() == FW_BOOT_RUN && a_is(s_new_img));
    boot_safe();
    printf("clean update: %ld frames, installed, confirmed, reported\n", clean_frames);

    /* 2. Never confirmed (hangs, or can't talk): three tries, then the old firmware is back */
    fresh_unit();
    assert(fw_ctrl_send_image("sensor.hex", &info, NULL) == FW_OK);
    assert(boot() == FW_BOOT_RUN_TRIAL);
    assert(boot() == FW_BOOT_RUN_TRIAL);
    assert(boot() == FW_BOOT_RUN_TRIAL);
    assert(boot() == FW_BOOT_RUN);
    assert(a_is(s_old_img) && b_is(s_new_img));
    fw_journal_read(&rec); assert(rec.type == FW_J_ROLLED_BACK);
    fw845_recv_init("845-old", s_old_crc);
    assert(!fw845_on_trial());
    fw_ctrl_tx("$UK;\n");
    fw_journal_read(&rec); assert(rec.type == FW_J_REPORTED);
    puts("never confirmed: old firmware back after 3 tries, and reported");

    /* 3. A bad link: lost commands, lost replies, damaged lines */
    long runs = 0;
    for (int every = 7; every <= 40; every += 3) {
        fresh_unit(); s_lose_cmd_every = every;
        assert(fw_ctrl_send_image("sensor.hex", &info, NULL) == FW_OK);
        assert(b_is(s_new_img)); assert(a_is(s_old_img)); runs++;
        fresh_unit(); s_lose_reply_every = every;
        assert(fw_ctrl_send_image("sensor.hex", &info, NULL) == FW_OK);
        assert(b_is(s_new_img)); runs++;
        fresh_unit(); s_damage_every = every;
        assert(fw_ctrl_send_image("sensor.hex", &info, NULL) == FW_OK);
        assert(b_is(s_new_img)); runs++;
    }
    printf("bad link: %ld runs with lost and damaged lines, every image arrived exact\n", runs);

    /* 4. The link dies at any point: running firmware untouched, nothing marked to install */
    runs = 0;
    for (long dead = 0; dead <= clean_frames; dead += 9) {
        fresh_unit(); s_dead_after = dead;
        fw_result_t r = fw_ctrl_send_image("sensor.hex", &info, NULL);
        if (r != FW_OK) {
            assert(r == FW_E_LINK);
            fw_journal_read(&rec); assert(rec.type == FW_J_NONE || dead >= clean_frames - 3);
        }
        assert(a_is(s_old_img));
        s_dead_after = -1;
        fw_ctrl_tx("$UR;\n");
        fw_journal_read(&rec);
        if (rec.type == FW_J_NONE) assert(!s_reset_requested); /* A stray restart is refused */
        runs++;
    }
    printf("link dies: %ld points, running firmware untouched\n", runs);

    /* 5. Power cut at every flash write during the transfer */
    fresh_unit();
    s_ops_done = 0;
    assert(fw_ctrl_send_image("sensor.hex", &info, NULL) == FW_OK);
    long ops = s_ops_done;
    runs = 0;
    for (long cut = 0; cut < ops; cut++) {
        fresh_unit();
        s_ops_left = cut;
        if (setjmp(s_cut) == 0) {
            (void)fw_ctrl_send_image("sensor.hex", &info, NULL);
        }
        s_ops_left = -1;
        assert(a_is(s_old_img));
        /* Restarting after the cut must leave a working unit, whatever got written */
        fw_boot_result_t r = boot();
        assert(r == FW_BOOT_RUN || r == FW_BOOT_RUN_TRIAL);
        runs++;
    }
    printf("power cut in the transfer: %ld points, always boots\n", runs);

    /* 6. Power cut at every flash write of the install, then on through a healthy trial */
    fresh_unit();
    assert(fw_ctrl_send_image("sensor.hex", &info, NULL) == FW_OK);
    s_ops_done = 0;
    assert(boot() == FW_BOOT_RUN_TRIAL);
    ops = s_ops_done;
    runs = 0;
    for (long cut = 0; cut < ops; cut++) {
        fresh_unit();
        assert(fw_ctrl_send_image("sensor.hex", &info, NULL) == FW_OK);
        s_ops_left = cut;
        fw_boot_result_t r = FW_BOOT_RETRY;
        if (setjmp(s_cut) == 0) {
            r = boot();
        }
        s_in_boot = false;
        s_ops_left = -1;
        for (int t = 0; t < 8 && r == FW_BOOT_RETRY; t++) r = boot();
        assert(r == FW_BOOT_RUN_TRIAL);
        assert(a_is(s_new_img) && b_is(s_old_img));
        boot_safe();
        runs++;
    }
    printf("power cut in the install: %ld points, always finishes\n", runs);

    /* 7. A file with a single wrong digit in the middle */
    size_t pos = s_file_len / 2;
    while (s_file[pos] != '\n') pos++;
    pos += 12;
    uint8_t keep = s_file[pos];
    s_file[pos] = (keep == '0') ? '1' : '0';
    assert(fw_ctrl_check("sensor.hex", &info, NULL) == FW_E_BAD_LINE);
    s_file[pos] = keep;
    /* And the file changes between the check and the send */
    assert(fw_ctrl_check("sensor.hex", &info, NULL) == FW_OK);
    info.crc32 ^= 1;
    fresh_unit();
    assert(fw_ctrl_send_image("sensor.hex", &info, NULL) != FW_OK);
    fw_journal_read(&rec); assert(rec.type == FW_J_NONE);
    assert(a_is(s_old_img));
    puts("damaged or changed file: refused, nothing marked");

    /* 8. Update after update: every update's journal stays good */
    assert(fw_ctrl_check("sensor.hex", &info, NULL) == FW_OK);
    fresh_unit();
    for (int round = 0; round < 40; round++) {
        fw845_recv_init("x", 0);
        assert(fw_ctrl_send_image("sensor.hex", &info, NULL) == FW_OK);
        assert(boot() == FW_BOOT_RUN_TRIAL);
        fw845_recv_init(info.version, info.crc32);
        fw_ctrl_tx("$UC;\n");
        fw_ctrl_tx("$UK;\n");
        assert(a_is(s_new_img));
        assert(boot() == FW_BOOT_RUN);
    }
    puts("40 updates in a row: journal stays good");

    puts("ALL PASS");
    return 0;
}
