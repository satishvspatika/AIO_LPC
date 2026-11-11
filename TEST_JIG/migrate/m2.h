/* M2: LPC1768 to the new bootloader and production firmware (TEST_JIG.md 13). */
#ifndef M2_H
#define M2_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    M2_DONE = 0,          /* PENDING written: restart to install */
    M2_FAIL_FILE,         /* Production file missing or unreadable: nothing unsafe done */
    M2_FAIL_IMAGE,        /* Staged image bad or for another type: nothing unsafe done */
    M2_FAIL_FLASH,
    M2_FAIL_BOOT_SECTOR,  /* Sector 0 write failed: needs the jig */
    M2_FAIL_TRIAL         /* Installed firmware failed its trial and was rolled back */
} m2_result_t;

typedef void (*m2_note_fn)(const char *text);

/* 0 KSNDMC_TRG, 1 BIHAR_TRG, 2 KSNDMC_TWS (UNIT_CFG values), -1 unknown. SD card mounted. */
int m2_detect_type(m2_note_fn note);
const char *m2_type_name(int type);
m2_result_t m2_convert(int type, m2_note_fn note);

#endif
