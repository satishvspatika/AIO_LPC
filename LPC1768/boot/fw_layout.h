/* LPC1768 flash map for firmware update. Shared by the bootloader, the application and the tests.
 *
 *   0x00000  sector 0       boot stub, jumps to the bootloader
 *   0x01000  sector 1       settings (station ID and so on), never touched by an update
 *   0x02000  sectors 2-11   bootloader, never changed by an update
 *   0x0C000  sectors 12-15  update journal (a ring of 64 records)
 *   0x10000  sector 16      swap scratch
 *   0x18000  sectors 17-20  firmware that runs (slot A)
 *   0x38000  sectors 21-24  staging (slot B): the new image, then the old one after a swap
 *   0x58000  sectors 25-29  record log
 */
#ifndef FW_LAYOUT_H
#define FW_LAYOUT_H

#include <stdint.h>
#include <stdbool.h>

#define FW_SECTOR_SIZE      0x8000UL
#define FW_SLOT_SECTORS     4UL
#define FW_SLOT_SIZE        (FW_SLOT_SECTORS * FW_SECTOR_SIZE)
#define FW_SCRATCH_BASE     0x00010000UL
#define FW_SCRATCH_SECTOR   16UL
#define FW_SLOT_A_BASE      0x00018000UL
#define FW_SLOT_A_SECTOR    17UL
#define FW_SLOT_B_BASE      0x00038000UL
#define FW_SLOT_B_SECTOR    21UL

#define FW_JOURNAL_BASE     0x0000C000UL
#define FW_JOURNAL_SECTOR   12UL
#define FW_JOURNAL_SECTORS  4UL
#define FW_JOURNAL_SECTOR_SIZE 0x1000UL
#define FW_JOURNAL_PAGE     256UL
#define FW_JOURNAL_PAGES    ((FW_JOURNAL_SECTORS * FW_JOURNAL_SECTOR_SIZE) / FW_JOURNAL_PAGE)

/* Every image says what it is. Reserved vector 8 points to this block, and scripts/fw_image.py
 * fills in the length and CRC after linking. */
#define FW_IMAGE_MAGIC      0x31474D49UL /* 'IMG1' */
#define FW_BOARD_ID         0x00001768UL

typedef struct {
    uint32_t magic;
    uint32_t board;
    uint32_t length;    /* Bytes of image from its start */
    uint32_t crc32;     /* Over the image, with this field read as zero */
    uint32_t profile;   /* UNIT_CFG */
    char version[16];
} fw_image_info_t;

#define FW_VECTOR_IMAGE_INFO 8

/* Flash access, supplied by the bootloader, the application or the tests */
const uint8_t *fw_hal_ptr(uint32_t addr);
bool fw_hal_erase(uint32_t sector);
bool fw_hal_program(uint32_t addr, const void *src, uint32_t len); /* 256 or 4096 bytes, aligned */

uint32_t fw_crc32_update(uint32_t crc, const uint8_t *data, uint32_t len);

#endif /* FW_LAYOUT_H */
