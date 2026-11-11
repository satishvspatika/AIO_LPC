#ifndef IAP1768_H
#define IAP1768_H

#include <stdint.h>
#include <stdbool.h>

uint32_t iap1768_sector_of(uint32_t addr);
bool iap1768_erase(uint32_t first_sector, uint32_t last_sector);
bool iap1768_blank(uint32_t addr, uint32_t len);
/* len 256, 512, 1024 or 4096, addr aligned to len; read back after writing */
bool iap1768_program(uint32_t addr, const void *src, uint32_t len);

#endif
