#ifndef FLASH_NVRAM_H
#define FLASH_NVRAM_H

#include <stdint.h>
#include <stdbool.h>

#define PARAM_SECTOR        1
#define PARAM_SECTOR_ADDR   0x00001000
#define PARAM_MAGIC         0x5350544B  /* 'SPTK' */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    char stn_id[8];          /* 6 chars + null (e.g. "001922") */
    uint8_t system_type;     /* SYSTEM_TRG, SYSTEM_TWS */
    uint8_t unit_cfg;        /* CFG_KSNDMC_TRG, CFG_BIHAR_TRG, CFG_KSNDMC_TWS */
    float rf_resolution;     /* e.g. 0.25mm */
    uint8_t sd_debug_dump;   /* 1 = enabled, 0 = disabled */
    char reserved[238];      /* Pad to 256 bytes for IAP write alignment */
} station_nvram_params_t;

void flash_nvram_init(void);
const char *flash_nvram_get_station_id(void);
bool flash_nvram_set_station_id(const char *new_id);
bool flash_nvram_read(station_nvram_params_t *out_params);
bool flash_nvram_write(const station_nvram_params_t *in_params);

#endif /* FLASH_NVRAM_H */

