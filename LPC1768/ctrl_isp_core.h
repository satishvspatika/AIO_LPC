/* What goes into which LPC845 flash sector, and in what order, for the CTRL ROM loader route
 * (ctrl_isp.c). No hardware here: the sector operations come in as functions, so the host test
 * (tests/ctrl_isp_test.c) runs the same code against a simulated LPC845 flash with power cuts. */
#ifndef CTRL_ISP_CORE_H
#define CTRL_ISP_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include "fw_update.h"

#define ISP_SECTOR_BYTES    1024U
#define ISP_SECTORS         64U

typedef enum {
    ISP_OK = 0,
    ISP_NO_JUMPERS,
    ISP_FILE_BAD,       /* ctrl.hex missing or damaged */
    ISP_FILE_CHANGED,   /* Not the file checked in the menu */
    ISP_NO_LOADER,      /* The ROM loader didn't answer */
    ISP_UNLOCK,
    ISP_SECTOR0,        /* Rescue or final sector 0 didn't write */
    ISP_WRITE,
    ISP_VERIFY,
    ISP_RESULT_COUNT
} isp_result_t;

typedef struct {
    /* Sector operations on the LPC845. Each returns false on any failure. */
    bool (*erase)(uint32_t sector);
    bool (*program)(uint32_t sector, const uint8_t *data);     /* Into an erased sector */
    bool (*read)(uint32_t sector, uint8_t *out);
    bool (*blank)(uint32_t sector);                             /* True only if all 0xFF */
    void (*progress)(const char *what, uint32_t sector);        /* May be NULL */
    /* The images */
    const uint8_t *boot;        /* CTRL bootloader, from 0x0 */
    uint32_t boot_len;
    const uint8_t *rescue;      /* Rescue sector 0 */
    uint32_t rescue_len;
    const char *ctrl_file;      /* ctrl.hex, already checked: its info */
    const fw_file_info_t *ctrl_info;
} ctrl_isp_ops_t;

/* With the ROM loader session up and unlocked: rescue sector 0, sectors 1-63, sector 0, then the
 * whole chip compared. *fail_sector says where it stopped. */
isp_result_t ctrl_isp_program(const ctrl_isp_ops_t *ops, uint32_t *fail_sector);

#endif /* CTRL_ISP_CORE_H */
