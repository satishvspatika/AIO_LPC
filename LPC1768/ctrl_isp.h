/* CTRL chip with no bootloader (a replaced LPC845 board, blank or on old firmware): the LPC1768
 * puts the CTRL bootloader and ctrl.hex on it through the LPC845's ROM loader, with JP1 + JP2
 * fitted on the CTRL board (JP1 RST_845, JP2 -PGM_845). Same method as the migration image's M1
 * (TEST_JIG/migrate, proven on old unit 2114), moved here in v6.9.33.
 *
 * The ROM loader entry is timed (the rails float while the LPC845 is in reset), so it runs at boot,
 * before the RTOS starts: MENU > FIRMWARE UPDATE > CTRL F/W asks for it and restarts the LPC1768. */
#ifndef CTRL_ISP_H
#define CTRL_ISP_H

#include <stdint.h>
#include <stdbool.h>

/* Both jumpers fitted: a moment's pull-down on P0.5 and P1.0 loses to the CTRL board's pull-ups
 * only through a fitted jumper. Busy-waits about 10 ms. */
bool ctrl_isp_jumpers_fitted(void);

/* Marks the request (RTC GPREG0/1, kept across the restart) for ctrl.hex with this CRC. The
 * caller restarts the LPC1768. */
void ctrl_isp_request(uint32_t ctrl_crc32);

/* In main(), after storage_log_init() and before the tasks: runs the request if there is one, and
 * then restarts the LPC1768 (does not return). Returns at once otherwise. */
void ctrl_isp_boot_check(void);

/* From a task, once the card is up: logs how the last run ended (debug log and UPDATE.TXT). */
void ctrl_isp_log_result(void);

#endif /* CTRL_ISP_H */
