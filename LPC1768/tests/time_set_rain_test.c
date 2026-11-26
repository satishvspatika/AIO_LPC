/* Host test (7.02): MENU > SET DATE/TIME keeps the rain-day total when the new time is in the same rain
 * day as the clock it corrects, and restarts it at 0 otherwise. Runs the firmware's own code, taken by
 * bench/host_tests.sh: ui_display.c time_set_keeps_rain() and intermcu_protocol.c's date functions. */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "ts_funcs.inc"   /* with intermcu_protocol.c's RAIN_CLOSE_SLOT */
#include "keep_rule.inc"

static int s_fails = 0;
static void check(const char *what, const char *old_ts, const char *new_ts, bool want)
{
    bool got = time_set_keeps_rain(old_ts, new_ts);
    if (got != want) s_fails++;
    printf("%s %-58s %s\n", got == want ? "PASS" : "FAIL", what, got ? "kept" : "from 0");
}
int main(void)
{
    check("20 min slow, same afternoon", "20261126134000", "20261126140000", true);
    check("5 min fast, before 08:30", "20261126082500", "20261126082000", true);
    check("evening to just after midnight (same rain day)", "20261126235500", "20261127001000", true);
    check("08:40 -> 08:50 (crosses 08:45: new rain day)", "20261126084000", "20261126085000", false);
    check("08:50 -> 08:40 (back over 08:45)", "20261126085000", "20261126084000", false);
    check("wrong date: 3 Nov -> 26 Nov", "20261103140000", "20261126140000", false);
    check("one day out, same time", "20261125140000", "20261126140000", false);
    check("wrong month", "20261026100000", "20261126100000", false);
    check("old clock lost (year 2000)", "20000101001500", "20261126140000", false);
    check("old clock empty", "", "20261126140000", false);
    check("year end, same rain day", "20261231230000", "20270101070000", true);
    check("leap day, same rain day", "20280229100000", "20280229120000", true);
    printf("%s (%d failed)\n", s_fails ? "FAILED" : "ALL PASSED", s_fails);
    return s_fails != 0;
}
