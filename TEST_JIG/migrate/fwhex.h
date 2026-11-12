/* Writes this image back out as fw.hex, line by line, in exactly the form build.sh makes it
 * (objcopy ihex: type 02 segment records, 16-byte type 00 records, CRLF, end record), because the
 * old SD bootloader V1.1 deletes fw.hex once it has loaded it (TEST_JIG.md 12). */
#ifndef FWHEX_H
#define FWHEX_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    const uint8_t *data;
    uint32_t addr;       /* Load address of data[0] */
    uint32_t len;
    uint32_t off;
    uint32_t seg_at;     /* Address the last segment record was written for */
    bool done;
} fwhex_t;

void fwhex_start(fwhex_t *h, const uint8_t *data, uint32_t addr, uint32_t len);
/* Next line, CRLF included, into line (at least 48 bytes); returns its length, 0 after the end record */
int fwhex_next(fwhex_t *h, char *line);

#endif
