/* Health report (v6.9.60): short fields added to the OTA check at the unit's night and day check slots.
 * Field list and meaning: docs/SYSTEM_DESIGN.md 7.7; the office side reads them in ota_server/lpc_ota.py. */
#ifndef HEALTH_H
#define HEALTH_H

#include <stdbool.h>
#include <stdint.h>
#include "intermcu_protocol.h"

/* "&hs=..&bv=.." for the check's path. v6.9.62: sent once a day, at the day check (13:00 + offset), and
 * covers the last whole MET day (08:45 the day before to 08:30 today), counted from the unit's flash
 * record log (kept even when the card loses writes). pending: records still waiting to be sent.
 * Returns a static string (never NULL). */
const char *health_query(const telemetry_data_t *telem, uint32_t pending);

/* The server took a report: the restart counts start again */
void health_sent(void);

/* At boot, after a restart that wasn't a plain power-on: counted for the next report */
void health_note_restart(bool watchdog);

/* The counting of SLOTS.CSV rows (now only the time on per slot), for the host test: feed it rows */
typedef struct {
    int rows;           /* Distinct slots seen */
    long dur_sum;
    int dur_n;
    char last_slot[16];
} health_scan_t;
void health_scan_start(health_scan_t *s);
void health_scan_row(health_scan_t *s, const char *row, const char *now14);

/* The MET day count, for the host test: feed it records and error log lines */
typedef struct {
    char now[16];
    long since;         /* Seconds since this MET day began (its 08:45 slot) */
    char day[9];        /* Close date (YYYYMMDD) of the day reported, "" if not known */
    int recs;           /* Records of the day that were meant to be sent */
    int late;           /* ... of them not delivered in their own slot */
    int late_today;     /* Late records of the day now running */
    long bd_sum, bn_sum;
    int bd_n, bn_n;
    int bmin;           /* Hundredths of a volt */
    int nerr;
    uint32_t err_seq;
    char err[52];
} health_day_t;
void health_day_start(health_day_t *d, const char *now14);
void health_day_record(const char *record, bool sendable, bool late, void *ctx);
void health_day_error(health_day_t *d, const char *entry, uint32_t seq);

#endif /* HEALTH_H */
