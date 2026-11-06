#include "flash_nvram.h"
#include "user_config.h"
#include "LPC17xx.h"
#include <string.h>
#include <stddef.h>

#define IAP_LOCATION    0x1FFF1FF1

typedef void (*IAP_ENTRY)(unsigned long[], unsigned long[]);
static const IAP_ENTRY iap_call = (IAP_ENTRY)IAP_LOCATION;

static unsigned long s_iap_cmd[5];
static unsigned long s_iap_res[5];

static char s_active_station_id[16] = STATION_ID_DEFAULT;
static char s_active_last_logged[20] = "NO DATA YET";

/* Robust In-Application Programming (IAP) writer for Sector 1 (0x1000)
 * Uses clock fallback (100MHz CCLK -> 398MHz PLL timing used by legacy TWS firmware)
 * and read-back hardware verification with memcmp. */
static bool iap_write_sector1(const void *src_buf, size_t len)
{
    if (!src_buf || len != 256) return false;

    static const uint32_t clocks[] = { 100000, 398000, 12000 };

    for (size_t c = 0; c < sizeof(clocks) / sizeof(clocks[0]); c++) {
        uint32_t clk = clocks[c];

        /* Step 1: Prepare Sector 1 for Erase */
        s_iap_cmd[0] = 50;
        s_iap_cmd[1] = PARAM_SECTOR;
        s_iap_cmd[2] = PARAM_SECTOR;
        __disable_irq();
        iap_call(s_iap_cmd, s_iap_res);
        __enable_irq();
        if (s_iap_res[0] != 0) continue;

        /* Delay between prepare and erase */
        for (volatile int d = 0; d < 20000; d++);

        /* Step 2: Erase Sector 1 */
        s_iap_cmd[0] = 52;
        s_iap_cmd[1] = PARAM_SECTOR;
        s_iap_cmd[2] = PARAM_SECTOR;
        s_iap_cmd[3] = clk;
        __disable_irq();
        iap_call(s_iap_cmd, s_iap_res);
        __enable_irq();
        if (s_iap_res[0] != 0) continue;

        /* Critical: Settling delay after sector erase (~100-200ms) */
        for (volatile int d = 0; d < 300000; d++);

        /* Step 3: Prepare Sector 1 again before write */
        s_iap_cmd[0] = 50;
        s_iap_cmd[1] = PARAM_SECTOR;
        s_iap_cmd[2] = PARAM_SECTOR;
        __disable_irq();
        iap_call(s_iap_cmd, s_iap_res);
        __enable_irq();
        if (s_iap_res[0] != 0) continue;

        /* Delay between prepare and write */
        for (volatile int d = 0; d < 20000; d++);

        /* Step 4: Copy RAM to Flash (256 bytes) */
        s_iap_cmd[0] = 51;
        s_iap_cmd[1] = PARAM_SECTOR_ADDR;
        s_iap_cmd[2] = (unsigned long)src_buf;
        s_iap_cmd[3] = 256;
        s_iap_cmd[4] = clk;
        __disable_irq();
        iap_call(s_iap_cmd, s_iap_res);
        __enable_irq();

        /* Settling delay */
        for (volatile int d = 0; d < 50000; d++);

        /* Step 5: Read-back verification directly against physical Flash address */
        if (s_iap_res[0] == 0 && memcmp((const void *)PARAM_SECTOR_ADDR, src_buf, len) == 0) {
            return true;
        }
    }

    return false;
}

void flash_nvram_init(void)
{
    const char *flash_str = (const char *)PARAM_SECTOR_ADDR;
    /* Legacy / Factory layout: offset 0 contains station ID string (e.g. "001061") */
    if (flash_str[0] >= '0' && flash_str[0] <= '9') {
        int len = 0;
        while (len < 6 && flash_str[len] >= '0' && flash_str[len] <= '9') {
            len++;
        }
        if (len >= 4) {
            char temp_id[8] = "";
            strncpy(temp_id, flash_str, len);
            temp_id[len] = '\0';

            /* User-configured custom Station ID: preserve permanently across power cycles & flashes */
            strncpy(s_active_station_id, temp_id, sizeof(s_active_station_id) - 1);
            s_active_station_id[sizeof(s_active_station_id) - 1] = '\0';
        }
    } else {
        /* Blank/uninitialized sector: write default station ID */
        strncpy(s_active_station_id, STATION_ID_DEFAULT, sizeof(s_active_station_id) - 1);
        s_active_station_id[sizeof(s_active_station_id) - 1] = '\0';
        flash_nvram_set_station_id(STATION_ID_DEFAULT);
    }

    /* Boot sets Last Logged from the record log. Sector 1 holds only a stale copy from older firmware. */
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

/* LOC_CHECK_BEGIN */
/* A position counts only with the date saved beside it. This sector was first written by older
 * firmware with a different layout, so these bytes can hold old values that look like a position. */
static bool location_date_valid(const char *d)
{
    for (int i = 0; i < 10; i++) {
        bool ok = (i == 4 || i == 7) ? (d[i] == '-') : (d[i] >= '0' && d[i] <= '9');
        if (!ok) {
            return false;
        }
    }
    return d[0] == '2' && d[1] == '0' && d[10] == '\0';
}

static bool location_values_valid(int32_t lat, int32_t lon)
{
    return lat != 0 && lon != 0 && lat >= -90000000 && lat <= 90000000 && lon >= -180000000 && lon <= 180000000;
}
/* LOC_CHECK_END */

bool flash_nvram_get_location(int32_t *lat_udeg, int32_t *lon_udeg)
{
    const station_nvram_params_t *p = (const station_nvram_params_t *)PARAM_SECTOR_ADDR;
    int32_t lat, lon;
    memcpy(&lat, &p->gps_lat_udeg, sizeof(lat));
    memcpy(&lon, &p->gps_lon_udeg, sizeof(lon));
    if (!location_date_valid(p->gps_date) || !location_values_valid(lat, lon)) {
        return false;
    }
    if (lat_udeg) *lat_udeg = lat;
    if (lon_udeg) *lon_udeg = lon;
    return true;
}

/* The tag tells a real value from old bytes in this sector */
#define FW_PROFILE_TAG 0x54505200UL /* 'TPR' */

int flash_nvram_get_profile(void)
{
    const station_nvram_params_t *p = (const station_nvram_params_t *)PARAM_SECTOR_ADDR;
    uint32_t tag;
    memcpy(&tag, &p->profile_tag, sizeof(tag));
    return ((tag & 0xFFFFFF00UL) == FW_PROFILE_TAG) ? (int)(tag & 0xFFUL) : -1;
}

bool flash_nvram_set_profile(int profile)
{
    uint8_t sector_buf[256] __attribute__((aligned(4)));
    uint32_t tag = FW_PROFILE_TAG | ((uint32_t)profile & 0xFFUL);
    memcpy(sector_buf, (const void *)PARAM_SECTOR_ADDR, 256);
    memcpy(sector_buf + offsetof(station_nvram_params_t, profile_tag), &tag, sizeof(tag));
    return iap_write_sector1(sector_buf, 256);
}

const char *flash_nvram_get_location_date(void)
{
    const char *stored = (const char *)(PARAM_SECTOR_ADDR + offsetof(station_nvram_params_t, gps_date));
    return location_date_valid(stored) ? stored : "";
}

bool flash_nvram_set_location(int32_t lat_udeg, int32_t lon_udeg, const char *date)
{
    /* On the stack: this runs rarely, and permanent RAM is short */
    uint8_t sector_buf[256] __attribute__((aligned(4)));
    memcpy(sector_buf, (const void *)PARAM_SECTOR_ADDR, 256);

    memcpy(sector_buf + offsetof(station_nvram_params_t, gps_lat_udeg), &lat_udeg, sizeof(lat_udeg));
    memcpy(sector_buf + offsetof(station_nvram_params_t, gps_lon_udeg), &lon_udeg, sizeof(lon_udeg));
    size_t offset = offsetof(station_nvram_params_t, gps_date);
    size_t size = sizeof(((station_nvram_params_t *)0)->gps_date);
    memset(sector_buf + offset, 0, size);
    if (date) {
        strncpy((char *)(sector_buf + offset), date, size - 1);
    }
    return iap_write_sector1(sector_buf, 256);
}

const char *flash_nvram_get_calib_result(void)
{
    const char *stored = (const char *)(PARAM_SECTOR_ADDR + offsetof(station_nvram_params_t, calib_result));
    return (stored[0] >= '0' && stored[0] <= '9') ? stored : "";
}

bool flash_nvram_set_calib_result(const char *result)
{
    if (!result) return false;

    static uint8_t sector_buf[256] __attribute__((aligned(4)));
    memcpy(sector_buf, (const void *)PARAM_SECTOR_ADDR, 256);

    size_t offset = offsetof(station_nvram_params_t, calib_result);
    memset(sector_buf + offset, 0, sizeof(((station_nvram_params_t *)0)->calib_result));
    strncpy((char *)(sector_buf + offset), result, sizeof(((station_nvram_params_t *)0)->calib_result) - 1);

    return iap_write_sector1(sector_buf, 256);
}

void flash_nvram_note_last_logged(const char *last_logged_str)
{
    if (!last_logged_str || strlen(last_logged_str) < 5) return;
    strncpy(s_active_last_logged, last_logged_str, sizeof(s_active_last_logged) - 1);
    s_active_last_logged[sizeof(s_active_last_logged) - 1] = '\0';
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

    bool ok = iap_write_sector1(sector_buf, 256);
    if (ok) {
        strncpy(s_active_last_logged, last_logged_str, sizeof(s_active_last_logged) - 1);
        s_active_last_logged[sizeof(s_active_last_logged) - 1] = '\0';
    }
    return ok;
}

bool flash_nvram_set_station_id(const char *new_id)
{
    if (!new_id || strlen(new_id) < 4) return false;

    static uint8_t sector_buf[256] __attribute__((aligned(4)));
    /* Read existing 256 bytes from Sector 1 to preserve other station parameters */
    memcpy(sector_buf, (const void *)PARAM_SECTOR_ADDR, 256);

    /* Update station ID at offset 0 */
    memset(sector_buf, 0, 8);
    size_t id_len = strlen(new_id);
    if (id_len > 6) id_len = 6;
    strncpy((char *)sector_buf, new_id, id_len);
    sector_buf[id_len] = '\0';

    bool ok = iap_write_sector1(sector_buf, 256);
    if (ok) {
        /* Update active RAM cache ONLY when flash write was verified 100% in silicon */
        strncpy(s_active_station_id, (char *)sector_buf, sizeof(s_active_station_id) - 1);
        s_active_station_id[sizeof(s_active_station_id) - 1] = '\0';
    }
    return ok;
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

    return iap_write_sector1(&ram_copy, sizeof(ram_copy));
}
