/* Health report (v6.9.60; v6.9.62 once a day). The day check (13:00 + offset) carries a summary of the
 * last whole MET day (08:45 the day before to 08:30 today), so the office sees a station's data delivery,
 * battery and faults without a visit (user, 18 Nov):
 *   sent/bk   records of that day delivered, and how many of them by backlog (later than their slot)
 *   bd/bn     average battery 10:00-15:45 (day) and 22:00-03:45 (night); bmin the lowest that day
 *   ne/err    errors logged that day, and the newest one's text
 * These come from the unit's flash record and error logs, not the card: on 1922 (17 Nov 21:00) the card
 * lost every write of one slot, which counted from SLOTS.CSV would have looked like a missed slot.
 * Only the time on per slot still comes from SLOTS.CSV. The restart counts are kept in HEALTH.DAT,
 * written at a boot after a fault and after each report the server took. */
#include "health.h"
#include "user_config.h"
#include "storage_log.h"
#include "sim7600_driver.h"
#include "flash_nvram.h"
#include "fw_app.h"
#include "fw_update.h"
#include "flash_log.h"
#include "errlog.h"
#include "intermcu_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEALTH_TAIL_BYTES   9000    /* 96 rows of about 70 bytes, with room */
#define DAY_SEC             86400L

static char s_query[340];
static char s_chunk[257];
static char s_line[128];

/* "12.30" as hundredths of a volt (1230). No atof or %f: they pull in the C library's strtod and double
 * arithmetic, 10 KB and 2.5 KB, more than there is room for. */
static int centivolts(const char *p)
{
    int v = 0, digits = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p++ - '0');
    }
    v *= 100;
    if (*p == '.') {
        for (p++; *p >= '0' && *p <= '9' && digits < 2; p++, digits++) {
            v += (*p - '0') * (digits ? 1 : 10);
        }
    }
    return v;
}

void health_scan_start(health_scan_t *s)
{
    memset(s, 0, sizeof(*s));
}

/* One SLOTS.CSV row. Counts each slot once (a slot can have two rows), only the last 24 hours. */
void health_scan_row(health_scan_t *s, const char *row, const char *now14)
{
    const char *col[10];
    int n = 0;
    col[n++] = row;
    for (const char *p = row; *p && n < 10; p++) {
        if (*p == ',') {
            col[n++] = p + 1;
        }
    }
    if (n < 9 || strncmp(col[1], "20", 2) != 0 || col[2] - col[1] != 15) {
        return;     /* The header, a NO_CLOCK row, or a damaged line */
    }
    char slot[16];
    memcpy(slot, col[1], 14);
    slot[14] = '\0';
    const long age = intermcu_clock_drift_sec(slot, now14);
    if (age < 0 || age >= DAY_SEC - 450 || strcmp(slot, s->last_slot) == 0) {
        return;     /* Older than the 95 slots before this one, or the same slot again */
    }
    strcpy(s->last_slot, slot);
    s->rows++;
    const long dur = atol(col[6]);
    if (strncmp(col[8], "SCHEDULED", 9) == 0 && dur > 0) {
        s->dur_sum += dur;
        s->dur_n++;
    }
}

/* Only what the office page accepts: 0-9 A-Z a-z . , : - (others become '-'); a space as '+', which
 * the query reads as a space (v6.9.62), so a '+' of its own becomes '-' */
static void clean_copy(char *out, size_t size, const char *in)
{
    size_t n = 0;
    for (; in && *in && n < size - 1; in++) {
        const char c = *in;
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        c == '.' || c == ',' || c == ':' || c == '-';
        out[n++] = (c == ' ') ? '+' : ok ? c : '-';
    }
    out[n] = '\0';
}

/* ---- the last whole MET day, from the flash logs ---- */

static int two(const char *p)
{
    return (p[0] - '0') * 10 + (p[1] - '0');
}

void health_day_start(health_day_t *d, const char *now14)
{
    memset(d, 0, sizeof(*d));
    d->bmin = 9999;
    d->since = -1;
    if (strlen(now14) < 14) {
        return;
    }
    snprintf(d->now, sizeof(d->now), "%.14s", now14);
    const int hm = two(now14 + 8) * 60 + two(now14 + 10);
    /* Slots since 08:45 (sample number, as in the SD record), then the time into this slot */
    d->since = (long)(((hm / 15) + 61) % 96) * 900L + (long)(hm % 15) * 60L + two(now14 + 12);
    if (hm >= 8 * 60 + 45) {
        memcpy(d->day, now14, 8);       /* The day reported closed at 08:30 today */
    }
}

/* Which day a time is in: 1 the day reported, 0 the day running now, -1 neither or not known */
static int day_of(const health_day_t *d, const char *ts14)
{
    if (d->since < 0 || strncmp(ts14, d->now, 14) > 0) {
        return -1;      /* No clock, or a time after now */
    }
    const long age = intermcu_clock_drift_sec(ts14, d->now);
    if (age < 0) {
        return -1;
    }
    const long into = age - d->since;
    return (into <= 0) ? 0 : (into <= DAY_SEC) ? 1 : -1;
}

/* A record: "NN,YYYY-MM-DD,HH:MM,...,signal,battery" (TRG and TWS alike: the last two fields) */
void health_day_record(const char *record, bool sendable, bool late, void *ctx)
{
    health_day_t *d = (health_day_t *)ctx;
    if (strlen(record) < 20 || record[2] != ',' || record[7] != '-' || record[16] != ':') {
        return;
    }
    char ts[16];
    snprintf(ts, sizeof(ts), "%.4s%.2s%.2s%.2s%.2s00", record + 3, record + 8, record + 11, record + 14, record + 17);
    const int which = day_of(d, ts);
    if (which == 0) {
        d->late_today += (sendable && late);
        return;
    }
    if (which != 1) {
        return;
    }
    const int hm = two(ts + 8) * 60 + two(ts + 10);
    if (!d->day[0] && hm < 8 * 60 + 45) {
        memcpy(d->day, ts, 8);          /* A time before 08:45 is on the close date itself */
    }
    if (sendable) {
        d->recs++;
        d->late += late;
    }
    const char *bat = strrchr(record, ',');
    const char *sig = bat;
    while (sig > record && sig[-1] != ',') {
        sig--;
    }
    const int sig_dbm = atoi(sig);
    if (sig_dbm == -110 || sig_dbm == -112) {
        return;         /* Unit off: the battery is copied from before, not measured */
    }
    const int cv = centivolts(bat + 1);
    if (cv <= 200) {
        return;
    }
    if (cv < d->bmin) {
        d->bmin = cv;
    }
    if (hm >= 10 * 60 && hm <= 15 * 60 + 45) {
        d->bd_sum += cv;
        d->bd_n++;
    } else if (hm >= 22 * 60 || hm <= 3 * 60 + 45) {
        d->bn_sum += cv;
        d->bn_n++;
    }
}

/* An error log entry: "v6.9.61 [2026-11-18 01:31:09 +29s] [ERROR] text" */
void health_day_error(health_day_t *d, const char *entry, uint32_t seq)
{
    const char *when = strchr(entry, '[');
    const char *msg = strstr(entry, "] [ERROR] ");
    if (!when || !msg || strlen(when) < 21 || when[5] != '-' || when[11] != ' ') {
        return;         /* No date (clock not known yet) */
    }
    char ts[16];
    snprintf(ts, sizeof(ts), "%.4s%.2s%.2s%.2s%.2s%.2s", when + 1, when + 6, when + 9, when + 12, when + 15, when + 18);
    if (day_of(d, ts) != 1) {
        return;
    }
    d->nerr++;
    if (seq >= d->err_seq) {
        d->err_seq = seq;
        clean_copy(d->err, 49, msg + 10);  /* 48 characters */
        size_t n = strlen(d->err);
        while (n > 0 && d->err[n - 1] == '+') {
            d->err[--n] = '\0';
        }
    }
}

static void scan_day(health_day_t *d, const char *now14)
{
    health_day_start(d, now14);
    flash_log_visit_records(health_day_record, d);
    for (uint32_t i = 0; i < ERRLOG_ENTRIES; i++) {
        uint32_t seq = 0;
        const char *e = errlog_entry(i, &seq);
        if (e) {
            health_day_error(d, e, seq);
        }
    }
}

static int avg_cv(long sum, int n)
{
    return (int)((sum + n / 2) / n);
}

static void scan_slots(health_scan_t *s, const char *now14)
{
    uint32_t size = 0;
    health_scan_start(s);
    if (!fw_file_open(SLOT_LOG_FILENAME, &size)) {
        return;
    }
    const uint32_t from = (size > HEALTH_TAIL_BYTES) ? size - HEALTH_TAIL_BYTES : 0;
    bool skip_partial = (from > 0);
    size_t len = 0;
    if (fw_file_seek(from)) {
        int got;
        while ((got = fw_file_read(s_chunk, sizeof(s_chunk) - 1)) > 0) {
            for (int i = 0; i < got; i++) {
                const char c = s_chunk[i];
                if (c == '\n') {
                    s_line[len] = '\0';
                    if (!skip_partial) {
                        health_scan_row(s, s_line, now14);
                    }
                    skip_partial = false;
                    len = 0;
                } else if (c != '\r' && len < sizeof(s_line) - 1) {
                    s_line[len++] = c;
                }
            }
        }
    }
    fw_file_close();
}

/* HEALTH.DAT: "<watchdog restarts> <other fault restarts>" since the last report taken */
static void load_counts(unsigned *wdt, unsigned *rst)
{
    char text[24];
    *wdt = *rst = 0;
    if (storage_state_load(HEALTH_FILENAME, text, sizeof(text))) {
        sscanf(text, "%u %u", wdt, rst);
    }
}

static void save_counts(unsigned wdt, unsigned rst)
{
    char text[24];
    snprintf(text, sizeof(text), "%03u %03u\r\n", wdt > 999 ? 999 : wdt, rst > 999 ? 999 : rst);
    storage_state_save(HEALTH_FILENAME, text);     /* Fixed length: overwritten in place */
}

void health_note_restart(bool watchdog)
{
    unsigned wdt, rst;
    load_counts(&wdt, &rst);
    save_counts(wdt + (watchdog ? 1U : 0U), rst + (watchdog ? 0U : 1U));
}

void health_sent(void)
{
    save_counts(0, 0);
}


const char *health_query(const telemetry_data_t *telem, uint32_t pending)
{
    static health_scan_t scan;
    static health_day_t day;
    modem_status_t modem;
    char op[17], ctrl[12], hs[24], gps[28] = "", bd[12] = "", bn[12] = "";
    unsigned wdt, rst;

    scan_slots(&scan, telem->present_time);
    scan_day(&day, telem->present_time);
    /* What is still waiting: the queue sends oldest first, so it is the newest late records. Those of
     * the day now running are the newest; any more are the reported day's. */
    long wait = (long)pending - day.late_today;
    if (wait < 0) {
        wait = 0;
    }
    if (wait > day.late) {
        wait = day.late;
    }
    load_counts(&wdt, &rst);
    sim7600_get_status(&modem);
    clean_copy(op, sizeof(op), modem.carrier);
    clean_copy(ctrl, sizeof(ctrl), fw_app_845_version());

    snprintf(hs, sizeof(hs), "%s%s%s", wdt ? "WDOG." : "", rst ? "CRASH." : "", (wdt || rst) ? "" : "OK");
    if (hs[strlen(hs) - 1] == '.') {
        hs[strlen(hs) - 1] = '\0';
    }

    int32_t lat = 0, lon = 0;
    if (flash_nvram_get_location(&lat, &lon) && (lat != 0 || lon != 0)) {
        snprintf(gps, sizeof(gps), "&gps=%s%ld.%06ld,%s%ld.%06ld", lat < 0 ? "-" : "", labs((long)lat) / 1000000L,
                 labs((long)lat) % 1000000L, lon < 0 ? "-" : "", labs((long)lon) / 1000000L, labs((long)lon) % 1000000L);
    }
    if (day.bd_n) {
        const int v = avg_cv(day.bd_sum, day.bd_n);
        snprintf(bd, sizeof(bd), "&bd=%d.%02d", v / 100, v % 100);
    }
    if (day.bn_n) {
        const int v = avg_cv(day.bn_sum, day.bn_n);
        snprintf(bn, sizeof(bn), "&bn=%d.%02d", v / 100, v % 100);
    }

    const int bv = (int)(telem->bat1_volt * 100.0f + 0.5f);
    const int bmin = (day.bmin < 9999) ? day.bmin : bv;
    snprintf(s_query, sizeof(s_query),
             "&hs=%s&bv=%d.%02d&bmin=%d.%02d&sig=%d&net=%d&op=%s&day=%s&sent=%ld&bk=%ld%s%s&ne=%d%s%s"
             "&q=%lu&dur=%ld&rst=%u&wdt=%u&sd=1&ctrl=%s&clk=%ld%s",
             hs, bv / 100, bv % 100, bmin / 100, bmin % 100, modem.csq_dbm, sim7600_slot_network(), op,
             day.day, (long)day.recs - wait, (long)day.late - wait, bd, bn, day.nerr,
             day.err[0] ? "&err=" : "", day.err, (unsigned long)pending,
             scan.dur_n ? scan.dur_sum / scan.dur_n : 0L, rst, wdt, ctrl, sim7600_last_clock_drift(), gps);
    return s_query;
}
