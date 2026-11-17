/* Host test for health.c (v6.9.60): the health report built from a pretend SLOTS.CSV, card and unit.
 * Build and run from bench/host_tests.sh. */
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
    snprintf(now.present_time, sizeof(now.present_time), "20261117133102");
    now.bat1_volt = 12.62f;

    /* 1. A full day: 2 days of rows (only the last 95 slots before 13:30 count), with a gap, queued
     *    slots, a slot recorded twice (the OTA install restart), an operator slot and the header */
    s_csv_len = 0;
    s_csv_len += (size_t)snprintf(s_csv, sizeof(s_csv), "time,slot,result,reason,http_attempts,csq_dbm,duration_sec,battery_v,boot,backlog\r\n");
    const long end = 17L * 86400 + 13 * 3600 + 30 * 60;     /* This slot: 17 Nov 13:30 */
    int expect_ok = 0, expect_late = 0;
    for (long t = end - 2 * 86400; t < end; t += 900) {
        char s14[16];
        slot_name(s14, t);
        const bool counted = (end - t) <= 95 * 900;
        long k = (end - t) / 900;
        if (k == 10 || k == 11) continue;                                   /* Unit off: no rows */
        if (k == 20 || k == 21) { row(s14, "BACKLOG", "NO_REPLY", 86, "11.74", "SCHEDULED"); if (counted) expect_late++; continue; }
        if (k == 30) { row(s14, "SENT", "OK_NAME", 113, "11.40", "OPERATOR"); row(s14, "SENT", "OK_NAME", 2, "12.45", "SCHEDULED"); if (counted) expect_ok++; continue; }
        row(s14, "SENT", "OK_NAME", 20, k == 200 ? "10.00" : "12.30", "SCHEDULED");   /* 10.00: too old to count */
        if (counted) expect_ok++;
    }
    CHECK(s_csv_len > 9000);    /* So the read starts mid-line and must skip the partial one */
    const char *q = health_query(&now, 0);
    printf("%s\n", q);
    CHECK(atoi(field(q, "ok")) == expect_ok + 1);           /* + this slot */
    CHECK(atoi(field(q, "late")) == expect_late && expect_late == 2);
    CHECK(atoi(field(q, "miss")) == 96 - (expect_ok + 1) - expect_late);
    CHECK(atoi(field(q, "miss")) == 2);                     /* The two slots with the unit off */
    CHECK(!strcmp(field(q, "bmin"), "11.40"));              /* The operator slot's reading counts, the old 10.00 doesn't */
    CHECK(!strcmp(field(q, "bv"), "12.62"));
    CHECK(!strcmp(field(q, "hs"), "OK") && !strcmp(field(q, "rst"), "0") && !strcmp(field(q, "wdt"), "0"));
    CHECK(!strcmp(field(q, "op"), "BSNL-Mobile") && !strcmp(field(q, "net"), "4") && !strcmp(field(q, "sig"), "-71"));
    CHECK(!strcmp(field(q, "ctrl"), "v1.16") && !strcmp(field(q, "clk"), "34") && !strcmp(field(q, "q"), "0"));
    CHECK(atoi(field(q, "dur")) >= 19 && atoi(field(q, "dur")) <= 22);   /* Scheduled slots only */
    CHECK(field(q, "gps") == NULL && strlen(q) < 280);

    /* 2. Restarts: counted until a report is taken; codes in hs */
    health_note_restart(true); health_note_restart(false); health_note_restart(false);
    q = health_query(&now, 3);
    CHECK(!strcmp(field(q, "wdt"), "1") && !strcmp(field(q, "rst"), "2") && !strcmp(field(q, "hs"), "WDOG.CRASH"));
    CHECK(!strcmp(field(q, "q"), "3"));
    health_sent();
    q = health_query(&now, 0);
    CHECK(!strcmp(field(q, "hs"), "OK") && !strcmp(field(q, "wdt"), "0"));
    health_note_restart(true);
    q = health_query(&now, 0);
    CHECK(!strcmp(field(q, "hs"), "WDOG"));
    health_sent();

    /* 3. Location (south and west negative), a 2G slot, an operator name with odd characters */
    s_lat = -12345678; s_lon = 77583500; s_net = 2; s_carrier = "Airtel&IoT=1";
    q = health_query(&now, 0);
    CHECK(!strcmp(field(q, "gps"), "-12.345678,77.583500") && !strcmp(field(q, "net"), "2"));
    CHECK(!strcmp(field(q, "op"), "Airtel-IoT-1"));

    /* 4. No SLOTS.CSV, or an empty one: only this slot is known */
    s_have_csv = false;
    q = health_query(&now, 0);
    CHECK(!strcmp(field(q, "ok"), "1") && !strcmp(field(q, "miss"), "95") && !strcmp(field(q, "bmin"), "12.62"));
    s_have_csv = true; s_csv_len = 0;
    q = health_query(&now, 0);
    CHECK(!strcmp(field(q, "ok"), "1") && !strcmp(field(q, "dur"), "0"));

    /* 5. Damaged rows and a NO_CLOCK row are passed over */
    s_csv_len = 0;
    row("20261117131500", "SENT", "OK_NAME", 20, "12.10", "SCHEDULED");
    s_csv_len += (size_t)snprintf(s_csv + s_csv_len, 200, "NO_CLOCK,NO_CLOCK,LOST,INVALID_CLOCK,0,-113,0,0.00,SCHEDULED,0\r\n20261117,,,\r\ngarbage\r\n");
    q = health_query(&now, 0);
    CHECK(!strcmp(field(q, "ok"), "2") && !strcmp(field(q, "bmin"), "12.10"));

    printf("%s: health_test, %d checks, %d failed\n", s_fails ? "FAILED" : "ALL PASS", s_checks, s_fails);
    return s_fails ? 1 : 0;
}
