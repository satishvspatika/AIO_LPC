/* The LPC845's ROM loader (ISP), reached over the CTRL link with JP1 + JP2 fitted.
 * Survey stage: enter, hold the rails with a RAM stub, read-only queries, leave. */
#ifndef ISP845_H
#define ISP845_H

#include <stdint.h>
#include <stdbool.h>

#define ISP_RAW_MAX 400

typedef struct {
    uint32_t baud;
    uint32_t ms_to_sync;        /* From the end of the reset pulse to the first "Synchronized" */
    uint32_t ms_rails_floating; /* From the start of the reset pulse to the stub's "G" answered */
    int step;                   /* Last step reached: 1 sync, 2 echo off, 3 unlock, 4 stub written, 5 stub started, 6 resynced */
    char part_id[ISP_RAW_MAX];  /* Raw replies, lines joined with '|' */
    char boot_ver[ISP_RAW_MAX];
    char uid[ISP_RAW_MAX];
    char note[ISP_RAW_MAX];     /* Replies on the way in, or where it stopped */
} isp_survey_t;

/* Fast entry at 115200 (the LPC1768 is on hold-up power until the stub runs), stub, resync */
bool isp_enter(isp_survey_t *out);
/* J, K and N: part ID, boot code version, serial number */
void isp_read_ids(isp_survey_t *out);
/* -PGM_845 released (high), short RST_845 pulse: the LPC845 starts its own firmware again,
 * which drives the rail lines within a few ms */
void isp_leave(void);

#endif
