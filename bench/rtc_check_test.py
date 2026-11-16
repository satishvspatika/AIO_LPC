#!/usr/bin/env python3
"""CTRL v1.16 RTC read checks, run on the Mac.

Takes rtc_bcd_ok(), rtc_step_ok() and rtc_reads_agree() from LPC845/main.c (between RTC_CHECK_BEGIN
and RTC_CHECK_END) and runs the real code against good reads, damaged reads and the cases that must
still pass: the midnight carry, a clock just set, the first read after a start.
"""
import os, subprocess, sys, tempfile
R = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
ctrl = open(os.path.join(R, 'LPC845/main.c')).read()
code = ctrl[ctrl.index('/* RTC_CHECK_BEGIN */'):ctrl.index('/* RTC_CHECK_END */')]
src = r'''
#include <stdio.h>
#include <stdint.h>
#include <string.h>
typedef struct { uint8_t century, year, month, date, hour, minute, sec; } xDateTime_def;
''' + code + r'''
static int fails = 0;
#define CHECK(what, cond) do { if (!(cond)) { printf("FAIL: %s\n", what); fails++; } } while (0)
static xDateTime_def T(int yy, int mo, int dd, int hh, int mi, int ss)
{
    xDateTime_def t = {20, (uint8_t)yy, (uint8_t)mo, (uint8_t)dd, (uint8_t)hh, (uint8_t)mi, (uint8_t)ss};
    return t;
}
int main(void)
{
    /* Registers: sec, min, hour, weekday, date, month, year (BCD) */
    uint8_t good[7] = {0x59, 0x14, 0x10, 0x01, 0x16, 0x11, 0x26};
    CHECK("a real time is taken", rtc_bcd_ok(good));
    uint8_t zero[7] = {0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x00};
    CHECK("the RTC's power-on time (01/01/00 00:00:00) is a real time", rtc_bcd_ok(zero));
    struct { int reg; uint8_t v; const char *what; } bad[] = {
        {0, 0x5A, "seconds 5A"}, {0, 0x60, "seconds 60"}, {1, 0x7F, "minutes 7F"}, {1, 0x60, "minutes 60"},
        {2, 0x24, "hours 24"}, {2, 0x1B, "hours 1B"}, {4, 0x00, "date 00"}, {4, 0x32, "date 32"},
        {5, 0x00, "month 00"}, {5, 0x13, "month 13"}, {6, 0xA0, "year A0"}, {6, 0x2F, "year 2F"},
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        uint8_t r[7];
        memcpy(r, good, 7);
        r[bad[i].reg] = bad[i].v;
        CHECK(bad[i].what, !rtc_bcd_ok(r));
    }
    uint8_t wd[7];
    memcpy(wd, good, 7);
    wd[3] = 0xFF;
    CHECK("the week day register is ignored", rtc_bcd_ok(wd));

    xDateTime_def now = T(26, 11, 16, 10, 14, 30), next = T(26, 11, 17, 0, 0, 0);
    xDateTime_def rd;
    rd = T(26, 11, 16, 10, 14, 30); CHECK("same second", rtc_step_ok(&now, &rd, &next));
    rd = T(26, 11, 16, 10, 14, 31); CHECK("one second on", rtc_step_ok(&now, &rd, &next));
    rd = T(26, 11, 16, 10, 14, 33); CHECK("three seconds on", rtc_step_ok(&now, &rd, &next));
    rd = T(26, 11, 16, 10, 14, 34); CHECK("four seconds on needs a second read", !rtc_step_ok(&now, &rd, &next));
    rd = T(26, 11, 16, 10, 14, 29); CHECK("a second back needs a second read", !rtc_step_ok(&now, &rd, &next));
    rd = T(26, 11, 16, 10, 15, 30); CHECK("minute bit flipped (14 to 15: a false boundary)", !rtc_step_ok(&now, &rd, &next));
    rd = T(26, 11, 16, 10, 30, 30); CHECK("minute 14 read as 30", !rtc_step_ok(&now, &rd, &next));
    rd = T(26, 11, 16, 12, 14, 30); CHECK("hour bit flipped (10 to 12)", !rtc_step_ok(&now, &rd, &next));
    rd = T(26, 11, 16, 0, 14, 30);  CHECK("hour read as 0", !rtc_step_ok(&now, &rd, &next));
    rd = T(26, 11, 17, 10, 14, 30); CHECK("date one on, same time", !rtc_step_ok(&now, &rd, &next));
    rd = T(26, 12, 16, 10, 14, 30); CHECK("another month", !rtc_step_ok(&now, &rd, &next));
    xDateTime_def start = T(0, 0, 0, 0, 0, 0), start_next = T(0, 0, 1, 0, 0, 0);
    rd = T(26, 11, 16, 10, 14, 30); CHECK("first read after a start needs a second read", !rtc_step_ok(&start, &rd, &start_next));

    /* Every minute and hour carry of a day, and the midnight carry, read a second apart */
    for (long s = 0; s < 86400; s++) {
        xDateTime_def a = T(26, 12, 31, (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60));
        xDateTime_def an = T(27, 1, 1, 0, 0, 0);
        long s2 = s + 1;
        xDateTime_def b = (s2 == 86400) ? T(27, 1, 1, 0, 0, 0)
                                        : T(26, 12, 31, (int)(s2 / 3600), (int)(s2 / 60 % 60), (int)(s2 % 60));
        if (!rtc_step_ok(&a, &b, &an)) {
            printf("FAIL: a second on from %02d:%02d:%02d refused\n", a.hour, a.minute, a.sec);
            fails++;
            break;
        }
    }
    xDateTime_def late = T(26, 12, 31, 23, 59, 58), late_next = T(27, 1, 1, 0, 0, 0);
    rd = T(27, 1, 1, 0, 0, 1); CHECK("three seconds across midnight and the year", rtc_step_ok(&late, &rd, &late_next));
    rd = T(27, 1, 1, 0, 1, 0); CHECK("a minute across midnight needs a second read", !rtc_step_ok(&late, &rd, &late_next));

    xDateTime_def a = T(26, 11, 16, 10, 14, 59), b;
    b = T(26, 11, 16, 10, 14, 59); CHECK("two reads the same agree", rtc_reads_agree(&a, &b));
    b = T(26, 11, 16, 10, 15, 0);  CHECK("two reads a second apart agree", rtc_reads_agree(&a, &b));
    b = T(26, 11, 16, 10, 15, 1);  CHECK("two reads two seconds apart don't", !rtc_reads_agree(&a, &b));
    b = T(26, 11, 16, 11, 14, 59); CHECK("two reads an hour apart don't", !rtc_reads_agree(&a, &b));
    b = T(26, 11, 17, 10, 14, 59); CHECK("two reads a day apart don't", !rtc_reads_agree(&a, &b));
    b = T(26, 11, 16, 10, 14, 58); CHECK("a second read behind the first doesn't", !rtc_reads_agree(&a, &b));

    printf(fails ? "FAILED\n" : "ALL PASS\n");
    return fails != 0;
}
'''
with tempfile.TemporaryDirectory() as d:
    c = os.path.join(d, 'rtc_test.c')
    exe = os.path.join(d, 'rtc_test')
    open(c, 'w').write(src)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wno-unused-function', '-fsanitize=address,undefined', '-o', exe, c], check=True)
    sys.exit(subprocess.run([exe]).returncode)
