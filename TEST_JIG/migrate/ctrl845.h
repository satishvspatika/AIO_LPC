/* M1: the LPC845 gets the CTRL bootloader and firmware, through its ROM loader (TEST_JIG.md 13). */
#ifndef CTRL845_H
#define CTRL845_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    M1_DONE = 0,        /* Written and read back */
    M1_ALREADY,         /* Already the new CTRL: nothing written */
    M1_FAIL_READ,
    M1_FAIL_SECTOR0,    /* Rescue or final sector 0 did not write: the LPC845 may need the jig */
    M1_FAIL_ERASE,
    M1_FAIL_WRITE,
    M1_FAIL_VERIFY
} m1_result_t;

/* Called with each 1 KB sector as first read, while the LPC845 still holds its old firmware
 * (not called once the rescue sector or new firmware is there). Returns false to stop saving. */
typedef bool (*m1_save_fn)(uint32_t addr, const uint8_t *data, uint32_t len);
typedef void (*m1_note_fn)(const char *text);

/* The ROM loader session must be up (isp_enter). */
m1_result_t m1_program_ctrl(m1_save_fn save_old, m1_note_fn note);
const char *m1_ctrl_version(void);

#endif
