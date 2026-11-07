/* The LPC845's side of an update: the $U commands that bring a new image from the LPC1768 into
 * staging. The chip keeps counting rain throughout. Only a checked image is ever marked to install,
 * and the bootloader installs it at the next restart. */
#ifndef FW845_RECV_H
#define FW845_RECV_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    FW845_OK = 0,
    FW845_E_FORMAT,     /* 1: not a command this understands */
    FW845_E_CHECKSUM,   /* 2: the line was damaged on the way */
    FW845_E_OFFSET,     /* 3: data from ahead of where it has got to */
    FW845_E_FLASH,      /* 4: staging could not be written */
    FW845_E_SIZE,       /* 5: bigger than the slot, or more data than was announced */
    FW845_E_NOT_STARTED,/* 6: data with no transfer going on */
    FW845_E_IMAGE,      /* 7: the whole image failed its check */
    FW845_E_JOURNAL,    /* 8: could not record that it's ready */
    FW845_E_ON_TRIAL    /* 9: staging holds the firmware to fall back to */
} fw845_err_t;

#define FW845_MAX_CHUNK 48

/* Reads the journal, so call before the first command */
void fw845_recv_init(const char *running_version, uint32_t running_crc);

/* Not to feed the watchdog while this is true */
bool fw845_on_trial(void);

/* cmd is the frame from '$' to ';'. Fills reply, up to 64 bytes. Sets reset_now if the chip should
 * restart once the reply has gone. */
void fw845_recv_command(const char *cmd, char *reply, size_t reply_size, bool *reset_now);

#endif /* FW845_RECV_H */
