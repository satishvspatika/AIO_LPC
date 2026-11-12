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

/* MIG-1.4: the first conversion rewrites the settings sector (0x1000) to the station ID alone, so
 * the old server login does not stay on the unit. A tag at 0x1080 keeps what a rerun needs (the old
 * fields are gone by then): "MIGRATED T=<type> D=<MIGRATE folder>". The production firmware reads
 * only the ID at 0x1000; a keypad ID change keeps the first 256 bytes, so the tag stays (harmless). */
#define M2_TAG_ADDR   0x1080UL
#define M2_TAG_PREFIX "MIGRATED T="

/* 0 KSNDMC_TRG, 1 BIHAR_TRG, 2 KSNDMC_TWS (UNIT_CFG values), -1 unknown: from the old settings */
int m2_detect_type(m2_note_fn note);
const char *m2_type_name(int type);
const char *m2_type_label(int type);   /* For the LCD: "KSNDMC TRG" */
const char *m2_type_file(int type);    /* Production file on the card */
m2_result_t m2_convert(int type, const char *unit_dir, m2_note_fn note);

#endif
