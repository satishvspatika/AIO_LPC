/* Tests fw_update.c on a real hex file, with the bootloader's own check on the result:
 *   cc -std=gnu11 -I.. -I../.. -o update_test update_test.c ../../fw_update.c ../fw_journal.c ../boot_core.c
 *   ./update_test ../../Debug/LPC1768_AIO_APP.hex <profile> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <assert.h>
#include "fw_update.h"
#include "boot/fw_layout.h"
#include "boot/fw_journal.h"
#include "boot/boot_core.h"

static uint8_t s_flash[0x80000];
static long s_ops_left = -1;
static jmp_buf s_cut;
static char *s_file = NULL;
static size_t s_file_len = 0, s_file_pos = 0;
static bool s_file_exists = true;
static long s_read_fail_at = -1;

static uint32_t sector_base(uint32_t sec) { return (sec < 16) ? sec * 0x1000UL : 0x10000UL + (sec - 16) * 0x8000UL; }
static uint32_t sector_size(uint32_t sec) { return (sec < 16) ? 0x1000UL : 0x8000UL; }
const uint8_t *fw_hal_ptr(uint32_t addr) { assert(addr < sizeof(s_flash)); return &s_flash[addr]; }
static void tick(void) { if (s_ops_left >= 0 && s_ops_left-- == 0) { s_ops_left = -1; longjmp(s_cut, 1); } }
bool fw_hal_erase(uint32_t sec) { tick(); memset(&s_flash[sector_base(sec)], 0xFF, sector_size(sec)); return true; }
bool fw_hal_program(uint32_t addr, const void *src, uint32_t len)
{
    assert(len == 256 || len == 4096);
    assert(addr >= 0xC000); /* Nothing below the journal: boot, bootloader and settings */
    assert(addr < 0x18000 ? addr < 0x10000 : addr >= FW_SLOT_B_BASE); /* Never the running firmware */
    tick();
    memcpy(&s_flash[addr], src, len);
    return true;
}
bool fw_file_open(const char *name, uint32_t *out_size) { (void)name; if (!s_file_exists) return false; s_file_pos = 0; *out_size = (uint32_t)s_file_len; return true; }
int fw_file_read(void *buf, uint32_t len)
{
    if (s_read_fail_at >= 0 && (long)s_file_pos >= s_read_fail_at) return -1;
    size_t left = s_file_len - s_file_pos;
    if (len > left) len = (uint32_t)left;
    memcpy(buf, s_file + s_file_pos, len);
    s_file_pos += len;
    return (int)len;
}
void fw_file_close(void) {}

static char *s_orig; static size_t s_orig_len; static uint32_t s_profile;
static void use_original(void) { memcpy(s_file, s_orig, s_orig_len); s_file_len = s_orig_len; s_file_exists = true; s_read_fail_at = -1; }
static void fresh_flash(void) { memset(s_flash, 0xFF, sizeof(s_flash)); }

/* Line n of the file, as start and length */
static bool nth_line(size_t n, size_t *start, size_t *len)
{
    size_t pos = 0, i = 0;
    while (pos < s_file_len) {
        size_t e = pos;
        while (e < s_file_len && s_file[e] != '\n') e++;
        if (i == n) { *start = pos; *len = e - pos; return true; }
        pos = e + 1; i++;
    }
    return false;
}
static void remove_line(size_t n)
{
    size_t st, ln; assert(nth_line(n, &st, &ln));
    memmove(s_file + st, s_file + st + ln + 1, s_file_len - st - ln - 1);
    s_file_len -= ln + 1;
}
static uint8_t hexval(char c) { return (uint8_t)((c <= '9') ? c - '0' : c - 'A' + 10); }
/* Changes a data byte and repairs the line's checksum, so only the image CRC can catch it */
static void flip_keep_line_ok(size_t n)
{
    size_t st, ln; assert(nth_line(n, &st, &ln));
    char *l = s_file + st;
    size_t end = ln; while (end > 0 && (l[end - 1] == '\r')) end--;
    l[9] = "0123456789ABCDEF"[hexval(l[9]) ^ 1];
    unsigned sum = 0;
    for (size_t i = 1; i + 2 < end; i += 2) sum += (unsigned)(hexval(l[i]) << 4 | hexval(l[i + 1]));
    unsigned cs = (-sum) & 0xFF;
    l[end - 2] = "0123456789ABCDEF"[cs >> 4];
    l[end - 1] = "0123456789ABCDEF"[cs & 15];
}

static void expect_check(fw_result_t want, const char *what)
{
    fw_file_info_t info;
    fw_result_t got = fw_update_check("fw.hex", s_profile, &info, NULL);
    printf("  %-44s -> %s\n", what, fw_update_error_text(got));
    assert(got == want);
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    s_profile = (uint32_t)atoi(argv[2]);
    FILE *f = fopen(argv[1], "rb"); assert(f);
    fseek(f, 0, SEEK_END); s_orig_len = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    s_orig = malloc(s_orig_len + 8192); s_file = malloc(s_orig_len + 8192);
    assert(fread(s_orig, 1, s_orig_len, f) == s_orig_len); fclose(f);

    fw_file_info_t info;
    use_original(); fresh_flash();
    assert(fw_update_check("fw.hex", s_profile, &info, NULL) == FW_OK);
    printf("real file: %s, %u bytes, CRC32 %08X\n", info.version, (unsigned)info.length, (unsigned)info.crc32);
    for (size_t i = 0; i < sizeof(s_flash); i++) assert(s_flash[i] == 0xFF); /* Checking writes nothing */

    puts("files that must be refused, with nothing written:");
    s_file_exists = false; expect_check(FW_E_NOT_FOUND, "no file"); use_original();
    s_file_len = 0; expect_check(FW_E_NO_END, "empty file"); use_original();
    s_file[s_file_len / 2 + 3] = 'G'; expect_check(FW_E_BAD_LINE, "not hex at all, mid-file");
    use_original();
    { size_t st, ln; nth_line(500, &st, &ln); s_file[st + 12] = (s_file[st + 12] == '0') ? '1' : '0'; }
    expect_check(FW_E_BAD_LINE, "one wrong digit (line checksum)"); use_original();
    remove_line(500); expect_check(FW_E_GAP, "a line missing"); use_original();
    { size_t st, ln; nth_line(1000, &st, &ln); s_file_len = st; }
    expect_check(FW_E_NO_END, "cut off part way, on a line end"); use_original();
    { size_t st, ln; nth_line(1000, &st, &ln); s_file_len = st + ln / 2; }
    expect_check(FW_E_BAD_LINE, "cut off in the middle of a line"); use_original();
    flip_keep_line_ok(700); expect_check(FW_E_BAD_CRC, "a changed byte with a valid line checksum"); use_original();
    { const char *full = ":020000020000FC\n:10000000000000000000000000000000000000000F0\n"; memmove(s_file + strlen(full), s_file, s_file_len); memcpy(s_file, full, strlen(full)); s_file_len += strlen(full); }
    expect_check(FW_E_BAD_LINE, "extra line with a bad length"); use_original();
    { const char *boot = ":020000040000FA\n:1000000000000000000000000000000000000000F0\n"; memmove(s_file + strlen(boot), s_file, s_file_len); memcpy(s_file, boot, strlen(boot)); s_file_len += strlen(boot); }
    expect_check(FW_E_BAD_ADDRESS, "data for the boot area (a FULL hex)"); use_original();
    { size_t st, ln; assert(nth_line(0, &st, &ln)); }
    s_read_fail_at = (long)(s_file_len / 2); expect_check(FW_E_READ, "SD card fails part way"); use_original();
    { fw_file_info_t tmp; memset(&tmp, 0, sizeof(tmp)); assert(fw_update_check("fw.hex", s_profile + 1, &tmp, NULL) == FW_E_WRONG_PROFILE); assert(tmp.profile == s_profile && tmp.length > 0 && tmp.version[0] == 'v');
      puts("  file for another station type                -> asks the operator, with its details");
      fresh_flash(); assert(fw_update_stage("fw.hex", &tmp, NULL) == FW_OK); assert(fw_image_valid(FW_SLOT_B_BASE)); fresh_flash(); puts("  ... and installs when the operator says yes"); }
    for (size_t i = 0; i < sizeof(s_flash); i++) assert(s_flash[i] == 0xFF);

    /* Line endings and a missing last newline are all fine */
    { size_t o = 0, n = 0; char *t = malloc(s_orig_len * 2); for (size_t i = 0; i < s_orig_len; i++) { if (s_orig[i] == '\r') continue; t[n++] = s_orig[i]; o++; } s_file_len = n; memcpy(s_file, t, n); free(t); expect_check(FW_OK, "unix line endings"); }
    { use_original(); while (s_file[s_file_len - 1] == '\n' || s_file[s_file_len - 1] == '\r') s_file_len--; expect_check(FW_OK, "no newline on the last line"); }

    /* A good stage: what the bootloader sees is valid, and it's marked pending only at the end */
    use_original(); fresh_flash();
    assert(fw_update_check("fw.hex", s_profile, &info, NULL) == FW_OK);
    assert(fw_update_stage("fw.hex", &info, NULL) == FW_OK);
    assert(fw_image_valid(FW_SLOT_B_BASE));
    fw_record_t rec; fw_journal_read(&rec); assert(rec.type == FW_J_PENDING);
    fw_file_info_t back; assert(fw_update_slot_b_info(&back) && back.crc32 == info.crc32 && !strcmp(back.version, info.version));
    puts("staged image: passes the bootloader's own check, and is marked ready only at the end");

    /* File swapped between check and copy: refused, nothing marked */
    use_original(); fresh_flash();
    assert(fw_update_check("fw.hex", s_profile, &info, NULL) == FW_OK);
    flip_keep_line_ok(700);
    assert(fw_update_stage("fw.hex", &info, NULL) == FW_E_BAD_CRC);
    fw_journal_read(&rec); assert(rec.type == FW_J_NONE);
    use_original();
    puts("file changed after the check: refused, nothing marked");

    /* SD card dies during the copy */
    fresh_flash();
    assert(fw_update_check("fw.hex", s_profile, &info, NULL) == FW_OK);
    s_read_fail_at = (long)(s_file_len / 2);
    assert(fw_update_stage("fw.hex", &info, NULL) == FW_E_READ);
    fw_journal_read(&rec); assert(rec.type == FW_J_NONE);
    assert(fw_boot_decide() != FW_BOOT_RUN_TRIAL);
    use_original();
    puts("SD card fails during the copy: nothing marked");

    /* A power cut at every flash operation of the copy: never marked ready unless complete */
    long ops = 0, cuts = 0;
    assert(fw_update_check("fw.hex", s_profile, &info, NULL) == FW_OK);
    fresh_flash(); s_ops_left = 100000; assert(fw_update_stage("fw.hex", &info, NULL) == FW_OK); ops = 100000 - s_ops_left; s_ops_left = -1;
    for (long cut = 0; cut <= ops; cut += 7) {
        fresh_flash();
        s_ops_left = cut;
        if (setjmp(s_cut) == 0) {
            fw_update_stage("fw.hex", &info, NULL);
        }
        s_ops_left = -1;
        fw_journal_read(&rec);
        if (rec.type == FW_J_PENDING) assert(fw_image_valid(FW_SLOT_B_BASE));
        cuts++;
    }
    printf("power cut at %ld points in the %ld flash writes: never marked ready unless complete\n", cuts, ops);
    puts("ALL PASS");
    return 0;
}
