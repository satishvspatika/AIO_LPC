/* Sends ctrl.hex to the LPC845 over the link between the chips. The LPC845's running firmware
 * stores it in staging and proves it (LPC845/boot/fw845_recv.c), and its bootloader installs it. */
#ifndef FW_CTRL_H
#define FW_CTRL_H

#include "fw_update.h"

typedef struct {
    unsigned state;     /* The LPC845's newest journal record (fw_j_type_t) */
    uint32_t crc32;     /* Of the firmware it's running */
    char version[17];
} fw_ctrl_state_t;

/* Blocks a moment. False if the LPC845 doesn't answer: its firmware has no bootloader. */
bool fw_ctrl_query(fw_ctrl_state_t *out);

/* Check first. Clears the LPC845's staging, sends the file a chunk at a time, and has the LPC845
 * prove the whole image. A failure anywhere leaves its running firmware as it was. */
fw_result_t fw_ctrl_send_image(const char *name, const fw_file_info_t *checked, fw_progress_fn progress);

/* After a good send: the LPC845 restarts and installs it. Its rails drop while it does. */
void fw_ctrl_restart_lpc845(void);

/* $Uq,<state>,<crc>,<version>; */
bool fw_ctrl_parse_state(const char *reply, fw_ctrl_state_t *out);

/* Supplied by the application or the tests */
void fw_ctrl_tx(const char *frame);
/* Waits for the next $U reply of this kind ("$U<kind>,...;"), dropping any other */
bool fw_ctrl_wait_reply(char kind, char *reply, size_t size, uint32_t timeout_ms);

#endif /* FW_CTRL_H */
