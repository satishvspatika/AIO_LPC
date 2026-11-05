#ifndef FLASH_NVRAM_H
#define FLASH_NVRAM_H

#include <stdint.h>
#include <stdbool.h>

#define PARAM_SECTOR        1
#define PARAM_SECTOR_ADDR   0x00001000
#define PARAM_MAGIC         0x5350544B  /* 'SPTK' */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    char stn_id[8];          /* 6 chars + null (e.g. "001061") */
    uint8_t system_type;     /* 0 = TRG, 1 = TWS (NVRAM byte alignment) */
    uint8_t unit_cfg;        /* KSNDMC_TRG, BIHAR_TRG, KSNDMC_TWS */
    float rf_resolution;     /* e.g. 0.25mm */
    uint8_t sd_debug_dump;   /* 1 = enabled, 0 = disabled */
    char last_logged[20];    /* e.g. "04-11-2026 18:45" */
    char calib_result[20];   /* e.g. "2026-11-05 PASS" */
    char reserved[198];      /* Pad to 256 bytes for IAP write alignment */
} station_nvram_params_t;

void flash_nvram_init(void);
const char *flash_nvram_get_station_id(void);
bool flash_nvram_set_station_id(const char *new_id);
const char *flash_nvram_get_last_logged(void);
bool flash_nvram_set_last_logged(const char *last_logged_str);
/* RAM only: per-slot updates are kept on the SD card to avoid wearing out this flash sector */
void flash_nvram_note_last_logged(const char *last_logged_str);
/* Last field calibration result, or "" if never calibrated */
const char *flash_nvram_get_calib_result(void);
bool flash_nvram_set_calib_result(const char *result);
bool flash_nvram_read(station_nvram_params_t *out_params);
bool flash_nvram_write(const station_nvram_params_t *in_params);

#endif /* FLASH_NVRAM_H */

