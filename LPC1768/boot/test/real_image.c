/* Checks a built image with the bootloader's own validity code:
 *   cc -std=gnu11 -I.. -o real_image real_image.c ../boot_core.c ../fw_journal.c && ./real_image image.bin */
#include <stdio.h>
#include <string.h>
#include "boot_core.h"
static uint8_t s_flash[0x80000];
const uint8_t *fw_hal_ptr(uint32_t addr) { return &s_flash[addr]; }
bool fw_hal_erase(uint32_t sector) { return false; }
bool fw_hal_program(uint32_t addr, const void *src, uint32_t len) { return false; }
int main(int argc, char **argv)
{
    memset(s_flash, 0xFF, sizeof(s_flash));
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    size_t n = fread(&s_flash[FW_SLOT_A_BASE], 1, FW_SLOT_SIZE, f);
    fclose(f);
    memcpy(&s_flash[FW_SLOT_B_BASE], &s_flash[FW_SLOT_A_BASE], FW_SLOT_SIZE);
    bool a = fw_image_valid(FW_SLOT_A_BASE), b = fw_image_valid(FW_SLOT_B_BASE);
    s_flash[FW_SLOT_B_BASE + n / 2] ^= 1;
    bool bad = fw_image_valid(FW_SLOT_B_BASE);
    printf("%zu bytes: valid in slot A %d, slot B %d, after one flipped bit %d\n", n, a, b, bad);
    return (a && b && !bad) ? 0 : 1;
}
