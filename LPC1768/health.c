/* Health report (v6.9.60). Twice a day the unit's OTA check carries a short summary of the last 24 hours,
 * so the office sees a station's battery, network and data delivery without a visit.
 *
 * Most of it is read back from SLOTS.CSV, which every slot already writes (one row per slot:
 * time,slot,result,reason,http_attempts,csq_dbm,duration_sec,battery_v,boot,backlog), so nothing new is
 * written each slot. Only the restart counts are kept in a file of their own (HEALTH.DAT), written at a
 * boot after a fault and after each report the server took. */
#include "health.h"
#include "user_config.h"
#include "storage_log.h"
#include "sim7600_driver.h"
#include "flash_nvram.h"
#include "fw_app.h"
#include "fw_update.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEALTH_TAIL_BYTES   9000    /* 96 rows of about 70 bytes, with room */
#define DAY_SEC             86400L

static char s_query[300];
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
    s->bmin = 9999;
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
    if (strncmp(col[2], "SENT,", 5) == 0) {
        s->ok++;
    } else if (strncmp(col[2], "BACKLOG,", 8) == 0) {
        s->late++;
    }
    const long dur = atol(col[6]);
    if (strncmp(col[8], "SCHEDULED", 9) == 0 && dur > 0) {
        s->dur_sum += dur;
        s->dur_n++;
    }
    const int bat = centivolts(col[7]);
    if (bat > 200 && bat < s->bmin) {
        s->bmin = bat;
    }
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

/* Only what the office page accepts: 0-9 A-Z a-z . , : + - (others become '-') */
static void clean_copy(char *out, size_t size, const char *in)
{
    size_t n = 0;
    for (; in && *in && n < size - 1; in++) {
        const char c = *in;
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        c == '.' || c == ',' || c == ':' || c == '+' || c == '-';
        out[n++] = ok ? c : '-';
    }
    out[n] = '\0';
}

const char *health_query(const telemetry_data_t *telem, uint32_t pending)
{
    static health_scan_t scan;
    modem_status_t modem;
    char op[17], ctrl[12], hs[24], gps[28] = "";
    unsigned wdt, rst;

    scan_slots(&scan, telem->present_time);
    /* This slot: its record went out (the check runs only after a delivered slot), its row is written later */
    const int ok = scan.ok + 1;
    int miss = 96 - ok - scan.late;
    if (miss < 0) {
        miss = 0;
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
        snprintf(gps, sizeof(gps), "%s%ld.%06ld,%s%ld.%06ld", lat < 0 ? "-" : "", labs((long)lat) / 1000000L,
                 labs((long)lat) % 1000000L, lon < 0 ? "-" : "", labs((long)lon) / 1000000L, labs((long)lon) % 1000000L);
    }

    const int bv = (int)(telem->bat1_volt * 100.0f + 0.5f);
    const int bmin = (scan.bmin < 9999) ? scan.bmin : bv;
    snprintf(s_query, sizeof(s_query),
             "&hs=%s&bv=%d.%02d&bmin=%d.%02d&sig=%d&net=%d&op=%s&ok=%d&late=%d&miss=%d&q=%lu&dur=%ld"
             "&rst=%u&wdt=%u&sd=1&ctrl=%s&clk=%ld%s%s",
             hs, bv / 100, bv % 100, bmin / 100, bmin % 100,
             modem.csq_dbm, sim7600_slot_network(), op, ok, scan.late, miss, (unsigned long)pending,
             scan.dur_n ? scan.dur_sum / scan.dur_n : 0L, rst, wdt, ctrl, sim7600_last_clock_drift(),
             gps[0] ? "&gps=" : "", gps);
    return s_query;
}
