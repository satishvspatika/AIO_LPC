#include "flash_nvram.h"
#include "user_config.h"
#include <string.h>
#include <stddef.h>

#define IAP_LOCATION    0x1FFF1FF1
#define IAP_CLK_KHZ     100000

typedef void (*IAP_ENTRY)(unsigned long[], unsigned long[]);
static const IAP_ENTRY iap_call = (IAP_ENTRY)IAP_LOCATION;

static unsigned long s_iap_cmd[5];
static unsigned long s_iap_res[5];

static char s_active_station_id[16] = STATION_ID_DEFAULT;
static char s_active_last_logged[20] = "NO DATA YET";

void flash_nvram_init(void)
{
    const char *flash_str = (const char *)PARAM_SECTOR_ADDR;
    /* Legacy / Factory layout: offset 0 contains station ID string (e.g. "001922") */
    if (flash_str[0] >= '0' && flash_str[0] <= '9') {
        int len = 0;
        while (len < 6 && flash_str[len] >= '0' && flash_str[len] <= '9') {
            len++;
        }
        if (len >= 4) {
            strncpy(s_active_station_id, flash_str, len);
            s_active_station_id[len] = '\0';
        }
    } else {
        strncpy(s_active_station_id, STATION_ID_DEFAULT, sizeof(s_active_station_id) - 1);
        s_active_station_id[sizeof(s_active_station_id) - 1] = '\0';
    }

    /* Load Last Logged timestamp from Sector 1 if present */
    const char *flash_log = (const char *)(PARAM_SECTOR_ADDR + offsetof(station_nvram_params_t, last_logged));
    if (flash_log[0] >= '0' && flash_log[0] <= '9') {
        int log_len = 0;
        while (log_len < 16 && (flash_log[log_len] >= '0' && flash_log[log_len] <= '9' ||
                                flash_log[log_len] == '-' || flash_log[log_len] == ':' ||
                                flash_log[log_len] == ' ' || flash_log[log_len] == '/')) {
            log_len++;
        }
        if (log_len >= 10) {
            strncpy(s_active_last_logged, flash_log, log_len);
            s_active_last_logged[log_len] = '\0';
            return;
        }
    }
    strncpy(s_active_last_logged, "NO DATA YET", sizeof(s_active_last_logged) - 1);
}

const char *flash_nvram_get_station_id(void)
{
    return s_active_station_id;
}

const char *flash_nvram_get_last_logged(void)
{
    return s_active_last_logged;
}

bool flash_nvram_set_last_logged(const char *last_logged_str)
{
    if (!last_logged_str || strlen(last_logged_str) < 5) return false;

    static uint8_t sector_buf[256] __attribute__((aligned(4)));
    memcpy(sector_buf, (const void *)PARAM_SECTOR_ADDR, 256);

    /* Update last_logged at offset */
    size_t offset = offsetof(station_nvram_params_t, last_logged);
    memset(sector_buf + offset, 0, sizeof(((station_nvram_params_t *)0)->last_logged));
    strncpy((char *)(sector_buf + offset), last_logged_str, sizeof(((station_nvram_params_t *)0)->last_logged) - 1);

    /* Update RAM cache */
    strncpy(s_active_last_logged, last_logged_str, sizeof(s_active_last_logged) - 1);
    s_active_last_logged[sizeof(s_active_last_logged) - 1] = '\0';

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
    s_iap_cmd[2] = (unsigned long)sector_buf;
    s_iap_cmd[3] = 256;
    s_iap_cmd[4] = IAP_CLK_KHZ;
    iap_call(s_iap_cmd, s_iap_res);

    __asm volatile ("cpsie i" : : : "memory");

    return (s_iap_res[0] == 0);
}

bool flash_nvram_set_station_id(const char *new_id)
{
    if (!new_id || strlen(new_id) < 4) return false;

    static uint8_t sector_buf[256] __attribute__((aligned(4)));
    /* Read existing 256 bytes from Sector 1 to preserve other station parameters */
    memcpy(sector_buf, (const void *)PARAM_SECTOR_ADDR, 256);

    /* Update station ID at offset 0 */
    memset(sector_buf, 0, 8);
    strncpy((char *)sector_buf, new_id, 7);

    /* Update RAM copy */
    strncpy(s_active_station_id, new_id, sizeof(s_active_station_id) - 1);
    s_active_station_id[sizeof(s_active_station_id) - 1] = '\0';

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
    s_iap_cmd[2] = (unsigned long)sector_buf;
    s_iap_cmd[3] = 256;
    s_iap_cmd[4] = IAP_CLK_KHZ;
    iap_call(s_iap_cmd, s_iap_res);

    __asm volatile ("cpsie i" : : : "memory");

    return (s_iap_res[0] == 0);
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
    out_params->system_type = (UNIT_CFG == KSNDMC_TWS) ? 1 : 0;
    out_params->unit_cfg = UNIT_CFG;
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
