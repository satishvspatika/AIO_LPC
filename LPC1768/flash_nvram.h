#ifndef FLASH_NVRAM_H
#define FLASH_NVRAM_H

#include <stdint.h>
#include <stdbool.h>

#define PARAM_SECTOR        1
#define PARAM_SECTOR_ADDR   0x00001000
#define PARAM_MAGIC         0x5350544B  /* 'SPTK' */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    char stn_id[8];          /* The ID string really starts at offset 0 over magic: up to 8 digits + NUL */
    uint8_t system_type;     /* 0 = TRG, 1 = TWS (NVRAM byte alignment) */
    uint8_t unit_cfg;        /* KSNDMC_TRG, BIHAR_TRG, KSNDMC_TWS */
    float rf_resolution;     /* e.g. 0.25mm */
    uint8_t sd_debug_dump;   /* 1 = enabled, 0 = disabled */
    char last_logged[20];    /* e.g. "04-11-2026 18:45" */
    char calib_result[20];   /* e.g. "2026-11-05 PASS" */
    int32_t gps_lat_udeg;    /* Last GPS position, millionths of a degree (south negative) */
    int32_t gps_lon_udeg;    /* West negative */
    char gps_date[12];       /* e.g. "2026-11-05" */
    uint32_t profile_tag;    /* FW_PROFILE_TAG | UNIT_CFG of the firmware that last ran */
    uint32_t rain_reset_tag; /* RAIN_RESET_TAG when rain_reset_seq is set (offsets 83-90; MIG's tag is at 128) */
    uint32_t rain_reset_seq; /* Records up to this seq are before the last rain-total restart */
    char reserved[165];      /* To exactly 256 bytes, one IAP write (was 174: 257 bytes, one too many) */
} station_nvram_params_t;
_Static_assert(sizeof(station_nvram_params_t) == 256, "settings sector layout must be 256 bytes");

void flash_nvram_init(void);
/* What flash_nvram_init found, to log once the card is up */
typedef enum {
    NVRAM_OK,            /* Settings present */
    NVRAM_NEW_UNIT,      /* Blank and no records: the build default ID is expected */
    NVRAM_ID_RECOVERED,  /* Blank, ID taken from the newest stored record */
    NVRAM_ID_LOST        /* Blank, records present but no ID in them: running as the build default */
} nvram_init_state_t;
nvram_init_state_t flash_nvram_init_state(void);
const char *flash_nvram_get_station_id(void);
bool flash_nvram_set_station_id(const char *new_id);
const char *flash_nvram_get_last_logged(void);
bool flash_nvram_set_last_logged(const char *last_logged_str);
/* RAM only: per-slot updates are kept on the SD card to avoid wearing out this flash sector */
void flash_nvram_note_last_logged(const char *last_logged_str);
/* Last field calibration result, or "" if never calibrated */
const char *flash_nvram_get_calib_result(void);
bool flash_nvram_set_calib_result(const char *result);
/* Saved GPS position. False if there is none yet. */
bool flash_nvram_get_location(int32_t *lat_udeg, int32_t *lon_udeg);
const char *flash_nvram_get_location_date(void);
bool flash_nvram_set_location(int32_t lat_udeg, int32_t lon_udeg, const char *date);
/* The station type that last ran here, or -1 if none is recorded */
int flash_nvram_get_profile(void);
bool flash_nvram_set_profile(int profile);
/* Rain-day total restarted by hand (SET DATE/TIME): records up to this flash-log seq don't count
 * towards CUM_RF any more. 0 = none. Erase Data clears it with the log. */
uint32_t flash_nvram_get_rain_reset_seq(void);
bool flash_nvram_set_rain_reset_seq(uint32_t seq);
bool flash_nvram_read(station_nvram_params_t *out_params);
bool flash_nvram_write(const station_nvram_params_t *in_params);

#endif /* FLASH_NVRAM_H */

