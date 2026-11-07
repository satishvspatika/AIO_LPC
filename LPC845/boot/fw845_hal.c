/* LPC845 flash access through the chip's own ROM routines, for both the bootloader and the firmware.
 * They need 32 bytes at the top of RAM, which the linker scripts keep free. */
#include "fw_layout.h"
#include "LPC845.h"

#define IAP_LOCATION    0x0F001FF1UL
#define IAP_CCLK_KHZ    12000UL /* The chip's reset clock, which neither program changes */

typedef void (*iap_entry_t)(uint32_t[], uint32_t[]);
static const iap_entry_t iap_call = (iap_entry_t)IAP_LOCATION;
static uint32_t s_cmd[5];
static uint32_t s_res[5];

const uint8_t *fw_hal_ptr(uint32_t addr)
{
    return (const uint8_t *)addr;
}

/* Code can't run from flash during these, so no interrupt can be allowed to try */
static bool iap_run(void)
{
    __disable_irq();
    iap_call(s_cmd, s_res);
    __enable_irq();
    return s_res[0] == 0;
}

static bool iap_prepare(uint32_t sector)
{
    s_cmd[0] = 50;
    s_cmd[1] = sector;
    s_cmd[2] = sector;
    return iap_run();
}

bool fw_hal_erase(uint32_t sector)
{
    if (!iap_prepare(sector)) {
        return false;
    }
    s_cmd[0] = 52;
    s_cmd[1] = sector;
    s_cmd[2] = sector;
    s_cmd[3] = IAP_CCLK_KHZ;
    return iap_run();
}

bool fw_hal_program(uint32_t addr, const void *src, uint32_t len)
{
    if (!iap_prepare(addr / FW_SECTOR_SIZE)) {
        return false;
    }
    s_cmd[0] = 51;
    s_cmd[1] = addr;
    s_cmd[2] = (uint32_t)src;
    s_cmd[3] = len;
    s_cmd[4] = IAP_CCLK_KHZ;
    return iap_run();
}
