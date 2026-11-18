/* Host test for health.c (v6.9.60; v6.9.62 MET-day report): the health report built from a pretend
 * SLOTS.CSV, flash record log, error log and unit.
 * Build and run from bench/host_tests.sh. */
#include "health.h"
#include "user_config.h"
#include "storage_log.h"
#include "sim7600_driver.h"
#include "flash_nvram.h"
#include "fw_app.h"
#include "fw_update.h"
#include "flash_log.h"
#include "errlog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_fails = 0, s_checks = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_fails++; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

/* ---- time: seconds since 2026-01-01 for YYYYMMDDHHMMSS (November only is enough here) ---- */
static long secs(const char *t)
{
    int d = (t[6] - '0') * 10 + (t[7] - '0'), h = (t[8] - '0') * 10 + (t[9] - '0');
    int m = (t[10] - '0') * 10 + (t[11] - '0'), s = (t[12] - '0') * 10 + (t[13] - '0');
    return ((long)d * 24 + h) * 3600L + m * 60L + s;
}
long intermcu_clock_drift_sec(const char *a, const char *b)
{
    if (strlen(a) < 14 || strlen(b) < 14) return -1;
    long d = labs(secs(a) - secs(b));
    return d > 2 * 86400L ? -1 : d;
}

/* ---- card ---- */
static char s_csv[40000];
static size_t s_csv_len, s_pos;
static bool s_open = false, s_have_csv = true;
bool fw_file_open(const char *name, uint32_t *size)
{
    CHECK(!strcmp(name, SLOT_LOG_FILENAME) && !s_open);
    if (!s_have_csv) return false;
    s_open = true; s_pos = 0; *size = (uint32_t)s_csv_len;
    return true;
}
bool fw_file_seek(uint32_t pos) { CHECK(s_open); s_pos = pos; return pos <= s_csv_len; }
int fw_file_read(void *buf, uint32_t len)
{
    CHECK(s_open);
    size_t n = s_csv_len - s_pos < len ? s_csv_len - s_pos : len;
    memcpy(buf, s_csv + s_pos, n); s_pos += n;
    return (int)n;
}
void fw_file_close(void) { CHECK(s_open); s_open = false; }
static char s_state[64] = "";
bool storage_state_load(const char *name, char *out, size_t size)
{
    CHECK(!strcmp(name, HEALTH_FILENAME));
    if (!s_state[0]) return false;
    snprintf(out, size, "%s", s_state);
    return true;
}
bool storage_state_save(const char *name, const char *text)
{
    CHECK(!strcmp(name, HEALTH_FILENAME) && strlen(text) == 9);   /* Fixed length */
    snprintf(s_state, sizeof(s_state), "%s", text);
    return true;
}

/* ---- unit ---- */
static int s_net = 4;
static const char *s_carrier = "BSNL Mobile";
void sim7600_get_status(modem_status_t *m) { memset(m, 0, sizeof(*m)); m->csq_dbm = -71; snprintf(m->carrier, sizeof(m->carrier), "%s", s_carrier); }
int sim7600_slot_network(void) { return s_net; }
long sim7600_last_clock_drift(void) { return 34; }
static int32_t s_lat = 0, s_lon = 0;
bool flash_nvram_get_location(int32_t *lat, int32_t *lon) { *lat = s_lat; *lon = s_lon; return true; }
const char *fw_app_845_version(void) { return "v1.16"; }

/* ---- flash record log and error log ---- */
typedef struct { char rec[72]; bool sendable, late; } rec_t;
static rec_t s_recs[400];
static int s_nrec;
void flash_log_visit_records(flash_log_visit_fn fn, void *ctx)
{
    for (int i = 0; i < s_nrec; i++) fn(s_recs[i].rec, s_recs[i].sendable, s_recs[i].late, ctx);
}
static void add_rec(long t, const char *bat, int sig, bool sendable, bool late)    /* t: seconds since 2026-11-00 */
{
    long d = t / 86400, h = (t / 3600) % 24, m = (t / 60) % 60;
    int sample = (int)(((h * 60 + m) / 15 + 61) % 96);
    rec_t *r = &s_recs[s_nrec++];
    snprintf(r->rec, sizeof(r->rec), "%02d,2026-11-%02ld,%02ld:%02ld,000.00,006.00,%04d,%s", sample, d, h, m, sig, bat);
    r->sendable = sendable; r->late = late;
}
static const char *s_err[ERRLOG_ENTRIES];
static uint32_t s_err_seq[ERRLOG_ENTRIES];
const char *errlog_entry(uint32_t i, uint32_t *seq) { if (i >= ERRLOG_ENTRIES || !s_err[i]) return NULL; *seq = s_err_seq[i]; return s_err[i]; }

static void row(const char *slot14, const char *result, const char *reason, int dur, const char *bat, const char *boot)
{
    s_csv_len += (size_t)snprintf(s_csv + s_csv_len, sizeof(s_csv) - s_csv_len, "%.12s02,%s,%s,%s,1,-71,%d,%s,%s,0\r\n",
                                  slot14, slot14, result, reason, dur, bat, boot);
}
static void slot_name(char *out, long t)       /* t: seconds since 2026-11-00 00:00 */
{
    snprintf(out, 16, "202611%02ld%02ld%02ld%02ld", t / 86400, (t / 3600) % 24, (t / 60) % 60, t % 60);
}
static const char *field(const char *q, const char *name)
{
    static char v[64];
    char key[16];
    snprintf(key, sizeof(key), "&%s=", name);
    const char *p = strstr(q, key);
    if (!p) return NULL;
    p += strlen(key);
    size_t n = strcspn(p, "&");
    snprintf(v, sizeof(v), "%.*s", (int)n, p);
    return v;
}

int main(void)
{
    telemetry_data_t now;
    memset(&now, 0, sizeof(now));
    snprintf(now.present_time, sizeof(now.present_time), "20261118130102");
    now.bat1_volt = 12.62f;

    /* 1. The MET day 17 Nov 08:45 to 18 Nov 08:30 (k = 0..95), with records of the day before it and
     *    of today around it. Two slots missing (unit off), one stopped (no payload), two off-filled
     *    (late, battery copied: not counted), one late and sent, one late and still waiting. */
    const long day0 = 17L * 86400 + 8 * 3600 + 45 * 60;
    for (long t = day0 - 8 * 900; t < day0; t += 900) add_rec(t, "09.00", -80, true, true);   /* 17 Nov 06:45-08:30: the day before, not counted */
    for (int k = 0; k < 96; k++) {
        const long t = day0 + k * 900L;
        const char *bat = (k >= 5 && k <= 28) ? "12.50" : (k >= 53 && k <= 76) ? "11.50" : "12.00";
        if (k == 5 || k == 6) continue;
        if (k == 7) { add_rec(t, bat, -85, false, false); continue; }
        if (k == 40 || k == 41) { add_rec(t, "10.00", -112, true, true); continue; }
        add_rec(t, bat, -85, true, k == 50 || k == 94);
    }
    for (long t = day0 + 96 * 900L; t <= 18L * 86400 + 13 * 3600; t += 900)        /* Today, to 13:00 */
        add_rec(t, "13.00", -85, true, t == 18L * 86400 + 10 * 3600);
    s_err[0] = "v6.9.61 [2026-11-16 20:00:01 +30s] [ERROR] Old error"; s_err_seq[0] = 5;
    s_err[3] = "v6.9.61 [2026-11-17 20:00:01 +30s] [ERROR] SD card not available: records are being kept in flash"; s_err_seq[3] = 10;
    s_err[4] = "v6.9.61 [2026-11-18 05:00:01 +30s] [ERROR] Could not save last slot to SD card"; s_err_seq[4] = 12;
    s_err[5] = "v6.9.61 [2026-11-18 10:00:01 +30s] [ERROR] Today, not counted"; s_err_seq[5] = 13;
    s_err[6] = "v6.9.61 [T+00:00:02.950] [ERROR] No time"; s_err_seq[6] = 14;
    /* Time on per slot still from SLOTS.CSV */
    s_csv_len = 0;
    s_csv_len += (size_t)snprintf(s_csv, sizeof(s_csv), "time,slot,result,reason,http_attempts,csq_dbm,duration_sec,battery_v,boot,backlog\r\n");
    const long end = 18L * 86400 + 13 * 3600;
    for (long t = end - 2 * 86400; t < end; t += 900) {
        char s14[16];
        slot_name(s14, t);
        row(s14, "SENT", "OK_NAME", 20, "12.30", "SCHEDULED");
        if ((end - t) / 900 == 30) row(s14, "SENT", "OK_NAME", 113, "11.40", "OPERATOR");
    }
    CHECK(s_csv_len > 9000);    /* So the read starts mid-line and must skip the partial one */
    const char *q = health_query(&now, 2);  /* Today's late one and the day's k = 94 still waiting */
    printf("%s\n", q);
    CHECK(!strcmp(field(q, "day"), "20261118"));
    CHECK(atoi(field(q, "sent")) == 92);    /* 96 - 2 missing - 1 stopped - 1 waiting */
    CHECK(atoi(field(q, "bk")) == 3);       /* 2 off-filled + k = 50 */
    CHECK(!strcmp(field(q, "bd"), "12.50") && !strcmp(field(q, "bn"), "11.50"));
    CHECK(!strcmp(field(q, "bmin"), "11.50"));  /* The off-filled 10.00 and 16 Nov's 09.00 don't count */
    CHECK(!strcmp(field(q, "ne"), "2"));
    CHECK(!strcmp(field(q, "err"), "Could+not+save+last+slot+to+SD+card"));
    CHECK(!strcmp(field(q, "bv"), "12.62") && !strcmp(field(q, "q"), "2"));
    CHECK(!strcmp(field(q, "hs"), "OK") && !strcmp(field(q, "rst"), "0") && !strcmp(field(q, "wdt"), "0"));
    CHECK(!strcmp(field(q, "op"), "BSNL+Mobile") && !strcmp(field(q, "net"), "4") && !strcmp(field(q, "sig"), "-71"));
    CHECK(!strcmp(field(q, "ctrl"), "v1.16") && !strcmp(field(q, "clk"), "34"));
    CHECK(atoi(field(q, "dur")) == 20);     /* Scheduled slots only */
    CHECK(field(q, "gps") == NULL && field(q, "ok") == NULL && field(q, "miss") == NULL);

    /* 1b. Nothing waiting: all late ones delivered. More waiting than late: capped */
    q = health_query(&now, 0);
    CHECK(atoi(field(q, "sent")) == 93 && atoi(field(q, "bk")) == 4);
    q = health_query(&now, 50);
    CHECK(atoi(field(q, "sent")) == 89 && atoi(field(q, "bk")) == 0);

    /* 1c. Day and night averages are averages (rounded), not the last value */
    {
        health_day_t d;
        health_day_start(&d, "20261118130102");
        health_day_record("05,2026-11-17,10:00,000.00,006.00,-085,12.00", true, false, &d);
        health_day_record("06,2026-11-17,10:15,000.00,006.00,-085,12.01", true, false, &d);
        health_day_record("53,2026-11-17,22:00,000.00,006.00,-085,11.00", true, false, &d);
        health_day_record("54,2026-11-18,03:45,000.00,006.00,-104,11.03", true, true, &d);   /* Gap-filled: measured, counts */
        health_day_record("55,2026-11-18,04:00,000.00,006.00,-085,10.50", true, false, &d);  /* Neither window */
        health_day_record("56,2026-11-18,16:00,000.00,006.00,-085,12.00", true, false, &d);  /* After now: ignored */
        health_day_record("garbage", true, false, &d);
        CHECK(d.recs == 5 && d.late == 1 && d.bd_n == 2 && d.bn_n == 2 && d.bmin == 1050);
        CHECK((d.bd_sum + 1) / 2 == 1201 && (d.bn_sum + 1) / 2 == 1102);
        /* TWS record: the same last two fields */
        health_day_record("57,2026-11-17,11:00,025.1,071.3,02.4,0185,-071,12.40", true, false, &d);
        CHECK(d.recs == 6 && d.bd_n == 3);
    }

    /* 2. Restarts: counted until a report is taken; codes in hs */
    health_note_restart(true); health_note_restart(false); health_note_restart(false);
    q = health_query(&now, 3);
    CHECK(!strcmp(field(q, "wdt"), "1") && !strcmp(field(q, "rst"), "2") && !strcmp(field(q, "hs"), "WDOG.CRASH"));
    health_sent();
    q = health_query(&now, 0);
    CHECK(!strcmp(field(q, "hs"), "OK") && !strcmp(field(q, "wdt"), "0"));
    health_note_restart(true);
    q = health_query(&now, 0);
    CHECK(!strcmp(field(q, "hs"), "WDOG"));
    health_sent();

    /* 3. Location (south and west negative), a 2G slot, an operator name with odd characters, and the
     *    longest report fits (long error text, gps, faults) */
    s_lat = -12345678; s_lon = 77583500; s_net = 2; s_carrier = "Airtel&IoT=1 Long Name";
    s_err[7] = "v6.9.61 [2026-11-18 06:00:01 +30s] [ERROR] Slot 2026-11-16 16:30 was stored but did not finish (power lost?): queued to send";
    s_err_seq[7] = 20;
    health_note_restart(true); health_note_restart(false);
    q = health_query(&now, 999);
    printf("%s (%zu)\n", q, strlen(q));
    CHECK(!strcmp(field(q, "gps"), "-12.345678,77.583500") && !strcmp(field(q, "net"), "2"));
    CHECK(!strcmp(field(q, "op"), "Airtel-IoT-1+Lo")   /* 15 characters kept */);
    CHECK(strlen(field(q, "err")) == 48 && !strncmp(field(q, "err"), "Slot+2026-11-16+16:30+was+stored+but+did+not+fin", 48));
    CHECK(!strcmp(field(q, "ne"), "3"));
    CHECK(strlen(q) < 330 && strlen(q) + 70 < 400);   /* With the check's own fields, s_path is 400 */
    health_sent();
    s_err[7] = NULL;

    /* 4. A bench report at night (02:00): the day reported is the one that closed at 08:30 yesterday */
    snprintf(now.present_time, sizeof(now.present_time), "20261118020005");
    q = health_query(&now, 0);
    CHECK(!strcmp(field(q, "day"), "20261117"));
    CHECK(atoi(field(q, "sent")) == 8 && atoi(field(q, "bk")) == 8);     /* Only its last 8 (06:45 to 08:30) are held */
    CHECK(!strcmp(field(q, "ne"), "1") && !strcmp(field(q, "err"), "Old+error"));

    /* 5. No records, no errors, no SLOTS.CSV: zeros, no averages, lowest is now */
    s_nrec = 0; memset(s_err, 0, sizeof(s_err)); s_have_csv = false;
    snprintf(now.present_time, sizeof(now.present_time), "20261118130102");
    q = health_query(&now, 0);
    CHECK(!strcmp(field(q, "sent"), "0") && !strcmp(field(q, "bk"), "0") && !strcmp(field(q, "ne"), "0"));
    CHECK(field(q, "bd") == NULL && field(q, "bn") == NULL && field(q, "err") == NULL);
    CHECK(!strcmp(field(q, "bmin"), "12.62") && !strcmp(field(q, "dur"), "0") && !strcmp(field(q, "day"), "20261118"));

    /* 6. No clock: nothing counted */
    snprintf(now.present_time, sizeof(now.present_time), "NO_CLOCK");
    add_rec(day0, "12.00", -85, true, false);
    q = health_query(&now, 0);
    CHECK(!strcmp(field(q, "sent"), "0") && !strcmp(field(q, "day"), ""));

    printf("%s: health_test, %d checks, %d failed\n", s_fails ? "FAILED" : "ALL PASS", s_checks, s_fails);
    return s_fails ? 1 : 0;
}
