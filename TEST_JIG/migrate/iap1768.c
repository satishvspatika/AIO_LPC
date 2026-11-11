/* LPC1768 flash writes for the migration image (UM10360 chapter 32: IAP at 0x1FFF1FF1;
 * 50 prepare, 51 copy RAM to flash, 52 erase; the clock in kHz). Also the flash access that
 * LPC1768/boot/boot_core.c and fw_journal.c call (fw_hal_*), so M2 writes the update journal with
 * the production code itself. Interrupts are off during each call: the vector table is in flash. */
#include "iap1768.h"
#include "fw_layout.h"
#include "LPC17xx.h"
#include "system_LPC17xx.h"
#include <string.h>

#define IAP_ENTRY 0x1FFF1FF1UL
typedef void (*iap_fn_t)(unsigned long[], unsigned long[]);

static unsigned long s_cmd[5];
static unsigned long s_res[5];
/* The source must be in main RAM: the boot ROM refuses some others (see LPC1768/fw_app.c) */
static uint32_t s_src[4096 / 4];

static unsigned long iap(unsigned long c0, unsigned long c1, unsigned long c2, unsigned long c3, unsigned long c4)
{
    s_cmd[0] = c0; s_cmd[1] = c1; s_cmd[2] = c2; s_cmd[3] = c3; s_cmd[4] = c4;
    __disable_irq();
    ((iap_fn_t)IAP_ENTRY)(s_cmd, s_res);
    __enable_irq();
    return s_res[0];
}

uint32_t iap1768_sector_of(uint32_t addr)
{
    return (addr < 0x10000UL) ? (addr / 0x1000UL) : (16UL + (addr - 0x10000UL) / 0x8000UL);
}

bool iap1768_erase(uint32_t first, uint32_t last)
{
    uint32_t khz = SystemCoreClock / 1000UL;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (iap(50, first, last, 0, 0) == 0 && iap(52, first, last, khz, 0) == 0) {
            return true;
        }
    }
    return false;
}

bool iap1768_blank(uint32_t addr, uint32_t len)
{
    const uint32_t *p = (const uint32_t *)addr;
    for (uint32_t i = 0; i < len / 4U; i++) {
        if (p[i] != 0xFFFFFFFFUL) return false;
    }
    return true;
}

bool iap1768_program(uint32_t addr, const void *src, uint32_t len)
{
    if (len != 256 && len != 512 && len != 1024 && len != 4096) {
        return false;
    }
    uint32_t khz = SystemCoreClock / 1000UL;
    uint32_t sector = iap1768_sector_of(addr);
    memcpy(s_src, src, len);
    for (int attempt = 0; attempt < 3; attempt++) {
        if (iap(50, sector, sector, 0, 0) != 0) continue;
        if (iap(51, addr, (unsigned long)s_src, len, khz) != 0) continue;
        if (memcmp((const void *)addr, s_src, len) == 0) return true;
        break;   /* Written but different: flash isn't erased there, retrying won't help */
    }
    return false;
}

/* ---- for boot_core.c / fw_journal.c ---- */
const uint8_t *fw_hal_ptr(uint32_t addr)
{
    return (const uint8_t *)addr;
}

bool fw_hal_erase(uint32_t sector)
{
    return iap1768_erase(sector, sector);
}

bool fw_hal_program(uint32_t addr, const void *src, uint32_t len)
{
    return iap1768_program(addr, src, len);
}
