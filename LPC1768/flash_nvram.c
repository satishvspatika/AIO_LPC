#include "flash_nvram.h"
#include "user_config.h"
#include <string.h>

#define IAP_LOCATION    0x1FFF1FF1
#define IAP_CLK_KHZ     100000

typedef void (*IAP_ENTRY)(unsigned long[], unsigned long[]);
static const IAP_ENTRY iap_call = (IAP_ENTRY)IAP_LOCATION;

static unsigned long s_iap_cmd[5];
static unsigned long s_iap_res[5];

void flash_nvram_init(void)
{
    /* Nothing required */
}

bool flash_nvram_read(station_nvram_params_t *out_params)
{
    if (!out_params) return false;
    const station_nvram_params_t *flash_ptr = (const station_nvram_params_t *)PARAM_SECTOR_ADDR;

    if (flash_ptr->magic == PARAM_MAGIC) {
        memcpy(out_params, flash_ptr, sizeof(station_nvram_params_t));
        return true;
    }

    /* Sector 1 is blank / invalid: populate default factory parameters */
    memset(out_params, 0, sizeof(station_nvram_params_t));
    out_params->magic = PARAM_MAGIC;
    strncpy(out_params->stn_id, STATION_ID_DEFAULT, sizeof(out_params->stn_id) - 1);
    out_params->system_type = ACTIVE_SYSTEM;
    out_params->unit_cfg = ACTIVE_CONFIG;
    out_params->rf_resolution = RF_BUCKET_RESOLUTION;
    out_params->sd_debug_dump = SDC_DEBUG_DUMP_DEFAULT;
    return false;
}

bool flash_nvram_write(const station_nvram_params_t *in_params)
{
    if (!in_params) return false;

    /* Prepare buffer aligned to 256 bytes */
    station_nvram_params_t ram_copy;
    memcpy(&ram_copy, in_params, sizeof(station_nvram_params_t));
    ram_copy.magic = PARAM_MAGIC;

    /* Disable interrupts during Flash write */
    __asm volatile ("cpsid i" : : : "memory");

    /* 1. Prepare Sector 1 */
    s_iap_cmd[0] = 50;
    s_iap_cmd[1] = PARAM_SECTOR;
    s_iap_cmd[2] = PARAM_SECTOR;
    iap_call(s_iap_cmd, s_iap_res);
    if (s_iap_res[0] != 0) {
        __asm volatile ("cpsie i" : : : "memory");
        return false;
    }

    /* 2. Erase Sector 1 */
    s_iap_cmd[0] = 52;
    s_iap_cmd[1] = PARAM_SECTOR;
    s_iap_cmd[2] = PARAM_SECTOR;
    s_iap_cmd[3] = IAP_CLK_KHZ;
    iap_call(s_iap_cmd, s_iap_res);
    if (s_iap_res[0] != 0) {
        __asm volatile ("cpsie i" : : : "memory");
        return false;
    }

    /* 3. Prepare Sector 1 again before write */
    s_iap_cmd[0] = 50;
    s_iap_cmd[1] = PARAM_SECTOR;
    s_iap_cmd[2] = PARAM_SECTOR;
    iap_call(s_iap_cmd, s_iap_res);
    if (s_iap_res[0] != 0) {
        __asm volatile ("cpsie i" : : : "memory");
        return false;
    }

    /* 4. Copy RAM to Flash (256 bytes) */
    s_iap_cmd[0] = 51;
    s_iap_cmd[1] = PARAM_SECTOR_ADDR;
    s_iap_cmd[2] = (unsigned long)&ram_copy;
    s_iap_cmd[3] = 256;
    s_iap_cmd[4] = IAP_CLK_KHZ;
    iap_call(s_iap_cmd, s_iap_res);

    __asm volatile ("cpsie i" : : : "memory");

    return (s_iap_res[0] == 0);
}
