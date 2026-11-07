#include "fw_ctrl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK_BYTES     48      /* FW845_MAX_CHUNK */
#define CHUNK_TRIES     6
#define CHUNK_WAIT_MS   2500    /* A chunk can end a flash sector, and a status frame can go first */
#define ERASE_WAIT_MS   15000   /* All of staging */
#define PROVE_WAIT_MS   5000
#define QUERY_WAIT_MS   2500
#define SHORT_TRIES     3

/* Error numbers from fw845_err_t */
#define E845_CHECKSUM   2
#define E845_FLASH      4
#define E845_IMAGE      7

/* Adds the XOR field and the end, and sends. frame has room for it. */
static void send_checked(char *frame, size_t size)
{
    uint8_t x = 0;
    size_t len = strlen(frame);
    for (size_t i = 1; i < len; i++) {
        x ^= (uint8_t)frame[i];
    }
    snprintf(frame + len, size - len, ",%02X;\n", x);
    fw_ctrl_tx(frame);
}

bool fw_ctrl_parse_state(const char *reply, fw_ctrl_state_t *out)
{
    /* Version last, up to the ';' */
    char *end;
    if (strncmp(reply, "$Uq,", 4) != 0) {
        return false;
    }
    unsigned long state = strtoul(reply + 4, &end, 10);
    if (*end != ',') {
        return false;
    }
    unsigned long crc = strtoul(end + 1, &end, 16);
    if (*end != ',') {
        return false;
    }
    const char *version = end + 1;
    const char *stop = strchr(version, ';');
    if (!stop || (stop - version) > 16) {
        return false;
    }
    out->state = (unsigned)state;
    out->crc32 = (uint32_t)crc;
    memcpy(out->version, version, (size_t)(stop - version));
    out->version[stop - version] = '\0';
    return true;
}

bool fw_ctrl_query(fw_ctrl_state_t *out)
{
    char reply[64];
    for (int t = 0; t < SHORT_TRIES; t++) {
        fw_ctrl_tx("$UQ;\n");
        if (fw_ctrl_wait_reply('q', reply, sizeof(reply), QUERY_WAIT_MS) && fw_ctrl_parse_state(reply, out)) {
            return true;
        }
    }
    return false;
}

static fw_result_t error_from_845(int e)
{
    if (e == E845_FLASH) return FW_E_FLASH;
    if (e == E845_IMAGE) return FW_E_BAD_CRC;
    return FW_E_LINK;
}

static fw_result_t s_chunk_error;

/* One chunk, until the LPC845 says it has it. Replies say where it has got to, so a reply that was
 * lost just gets the same answer again. */
static bool send_chunk(uint32_t offset, const uint8_t *data, uint32_t len)
{
    char frame[128];
    char reply[64];
    static const char digits[] = "0123456789ABCDEF";

    int n = snprintf(frame, sizeof(frame), "$UD%08lX,", (unsigned long)offset);
    for (uint32_t i = 0; i < len; i++) {
        frame[n++] = digits[data[i] >> 4];
        frame[n++] = digits[data[i] & 0x0F];
    }
    frame[n] = '\0';
    size_t base_len = (size_t)n;

    for (int t = 0; t < CHUNK_TRIES; t++) {
        frame[base_len] = '\0';
        send_checked(frame, sizeof(frame));
        if (!fw_ctrl_wait_reply('a', reply, sizeof(reply), CHUNK_WAIT_MS)) {
            continue;
        }
        char *end;
        unsigned long next = strtoul(reply + 4, &end, 16);
        if (*end != ',') {
            continue;
        }
        int e = atoi(end + 1);
        if (e == 0 && next == offset + len) {
            return true;
        }
        if (e != E845_CHECKSUM && e != 0) {
            s_chunk_error = error_from_845(e);
            return false;
        }
    }
    s_chunk_error = FW_E_LINK;
    return false;
}

fw_result_t fw_ctrl_send_image(const char *name, const fw_file_info_t *checked, fw_progress_fn progress)
{
    char frame[48];
    char reply[64];

    if (progress) {
        progress("ERASING", 0);
    }
    bool begun = false;
    for (int t = 0; t < SHORT_TRIES && !begun; t++) {
        snprintf(frame, sizeof(frame), "$UB%08lX,%08lX", (unsigned long)checked->length, (unsigned long)checked->crc32);
        send_checked(frame, sizeof(frame));
        if (!fw_ctrl_wait_reply('b', reply, sizeof(reply), ERASE_WAIT_MS)) {
            continue;
        }
        int e = atoi(reply + 4);
        if (e == 0) {
            begun = true;
        } else if (e != E845_CHECKSUM) {
            return error_from_845(e);
        }
    }
    if (!begun) {
        return FW_E_LINK;
    }

    s_chunk_error = FW_E_LINK;
    fw_result_t r = fw_ctrl_stream(name, checked, send_chunk, CHUNK_BYTES, progress);
    if (r == FW_E_LINK) {
        return s_chunk_error;
    }
    if (r != FW_OK) {
        return r;
    }

    if (progress) {
        progress("VERIFYING", 0);
    }
    for (int t = 0; t < SHORT_TRIES; t++) {
        fw_ctrl_tx("$UE;\n");
        if (fw_ctrl_wait_reply('e', reply, sizeof(reply), PROVE_WAIT_MS)) {
            int e = atoi(reply + 4);
            return (e == 0) ? FW_OK : error_from_845(e);
        }
    }
    return FW_E_LINK;
}

void fw_ctrl_restart_lpc845(void)
{
    fw_ctrl_tx("$UR;\n");
}
