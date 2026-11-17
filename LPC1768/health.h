/* Health report (v6.9.60): short fields added to the OTA check at the unit's night and day check slots.
 * Field list and meaning: docs/SYSTEM_DESIGN.md 7.7; the office side reads them in ota_server/lpc_ota.py. */
#ifndef HEALTH_H
#define HEALTH_H

#include <stdbool.h>
#include <stdint.h>
#include "intermcu_protocol.h"

/* "&hs=..&bv=.." for the check's path, built now from the card's SLOTS.CSV and the unit's state.
 * pending: records still waiting to be sent. Returns a static string (never NULL). */
const char *health_query(const telemetry_data_t *telem, uint32_t pending);

/* The server took a report: the restart counts start again */
void health_sent(void);

/* At boot, after a restart that wasn't a plain power-on: counted for the next report */
void health_note_restart(bool watchdog);

/* The counting of SLOTS.CSV rows, for the host test: feed it rows, oldest first */
typedef struct {
    int ok;             /* Slots sent in their own slot */
    int late;           /* Slots queued for later */
    int rows;           /* Distinct slots seen */
    long dur_sum;
    int dur_n;
    int bmin;           /* Hundredths of a volt */
    char last_slot[16];
} health_scan_t;
void health_scan_start(health_scan_t *s);
void health_scan_row(health_scan_t *s, const char *row, const char *now14);

#endif /* HEALTH_H */
