#include "fwhex.h"

static char *put_hex(char *p, uint32_t v, int digits)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = digits - 1; i >= 0; i--) {
        p[i] = hex[v & 0xFU];
        v >>= 4;
    }
    return p + digits;
}

/* One record: count, 16-bit address, type, data, checksum, CRLF */
static int record(char *line, uint32_t addr16, uint8_t type, const uint8_t *data, uint32_t n)
{
    char *p = line;
    uint32_t sum = n + (addr16 >> 8) + (addr16 & 0xFFU) + type;
    *p++ = ':';
    p = put_hex(p, n, 2);
    p = put_hex(p, addr16, 4);
    p = put_hex(p, type, 2);
    for (uint32_t i = 0; i < n; i++) {
        p = put_hex(p, data[i], 2);
        sum += data[i];
    }
    p = put_hex(p, (0x100U - (sum & 0xFFU)) & 0xFFU, 2);
    *p++ = '\r';
    *p++ = '\n';
    return (int)(p - line);
}

void fwhex_start(fwhex_t *h, const uint8_t *data, uint32_t addr, uint32_t len)
{
    h->data = data;
    h->addr = addr;
    h->len = len;
    h->off = 0;
    h->seg_at = 0xFFFFFFFFUL;
    h->done = false;
}

int fwhex_next(fwhex_t *h, char *line)
{
    if (h->done) {
        return 0;
    }
    if (h->off >= h->len) {
        h->done = true;
        return record(line, 0, 1, 0, 0);
    }
    uint32_t a = h->addr + h->off;
    if (h->seg_at == 0xFFFFFFFFUL || ((a & 0xFFFFU) == 0 && h->seg_at != a)) {
        uint32_t seg16 = (a & 0xF0000UL) >> 4;   /* 0x18000 -> 0x1000, as objcopy */
        uint8_t seg[2] = {(uint8_t)(seg16 >> 8), (uint8_t)(seg16 & 0xFFU)};
        h->seg_at = a;
        return record(line, 0, 2, seg, 2);
    }
    uint32_t n = h->len - h->off;
    if (n > 16) n = 16;
    if ((a & 0xFFFFU) + n > 0x10000U) n = 0x10000U - (a & 0xFFFFU);
    int r = record(line, a & 0xFFFFU, 0, h->data + h->off, n);
    h->off += n;
    return r;
}
