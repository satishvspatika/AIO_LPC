/* Host test for net_policy.c: cc -std=gnu11 -I.. net_policy_test.c ../net_policy.c
 * Each scenario runs slots through a simulated site, with RATMODE.DAT written and read back between
 * power-ons as the firmware does, and checks the 4G search time spent and where slots were sent. */
#include "net_policy.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* What the site gives: does 4G register (and how fast), does data work on each mode */
typedef struct {
    bool has_4g; uint8_t sec_4g; bool data_4g;
    bool has_2g; bool data_2g;
} site_t;

typedef struct { unsigned wait4_total; unsigned sent4, sent2, lost; unsigned skipped4; unsigned rescues; } tally_t;

static char g_file[48] = "";
static const char *g_imsi = "404719180719647";   /* BSNL Karnataka, as on 1922 */

static void slot(const site_t *s, tally_t *t)
{
    net_policy_t p;
    net_policy_parse(&p, g_file);
    net_policy_set_operator(&p, g_imsi);
    net_plan_t plan = net_policy_plan(&p);
    int mode = 0;
    uint8_t sec = 0;
    bool searched_4g = plan.search_4g;
    if (plan.search_4g && s->has_4g && s->sec_4g < plan.wait_4g_sec) {
        mode = NET_4G; sec = s->sec_4g;
        t->wait4_total += s->sec_4g;
    } else {
        if (plan.search_4g) t->wait4_total += plan.wait_4g_sec; else t->skipped4++;
        if (s->has_2g) {
            mode = NET_2G;
        } else if (!plan.search_4g) {
            /* The driver's rescue: no 2G after NET_2G_ONLY_WAIT_SEC, so 4G for the rest of the search */
            searched_4g = true;
            t->rescues++;
            if (s->has_4g) { mode = NET_4G; sec = s->sec_4g; }
        }
    }
    net_policy_registered(&p, searched_4g, mode, sec);
    net_policy_format(&p, g_file, sizeof(g_file));      /* saved after registering */
    net_policy_parse(&p, g_file);
    if (mode) {
        bool ok = (mode == NET_4G) ? s->data_4g : s->data_2g;
        net_policy_delivery(&p, mode, ok, !ok);
        if (ok) { if (mode == NET_4G) t->sent4++; else t->sent2++; } else t->lost++;
    } else {
        t->lost++;
    }
    net_policy_format(&p, g_file, sizeof(g_file));
}

static tally_t run(const char *name, const site_t *s, int slots)
{
    tally_t t = { 0 };
    for (int i = 0; i < slots; i++) slot(s, &t);
    printf("%-44s 4G search %4us in %d slots (%.1f s/slot), 4G skipped %2u, sent 4G %2u 2G %2u, lost %2u  [%s]\n",
           name, t.wait4_total, slots, (double)t.wait4_total / slots, t.skipped4, t.sent4, t.sent2, t.lost,
           strtok(strcpy((char[48]){0}, g_file), "\r"));
    return t;
}

int main(void)
{
    const site_t good4g   = { true, 1, true,  true, true };
    const site_t only2g   = { false, 0, false, true, true };
    const site_t weak4g   = { true, 2, false, true, true };   /* registers on 4G, data fails */
    const site_t slow4g   = { true, 9, true,  true, true };
    const site_t nothing  = { false, 0, false, false, false };
    tally_t t;

    g_file[0] = '\0';
    t = run("Good 4G (1922 today)", &good4g, 96);
    CHECK(t.sent4 == 96 && t.wait4_total == 96, "good 4G: all on 4G, 1 s each");
    CHECK(strncmp(g_file, "P4 ", 3) == 0, "good 4G stays 4G primary");

    g_file[0] = '\0';
    t = run("2G-only site, from a new unit", &only2g, 96);
    CHECK(t.sent2 == 96, "2G-only: every slot sent on 2G");
    CHECK(t.wait4_total <= 16 + 2 * 6 + 3 * 6 + 3 * 20 + 6, "2G-only: 4G search bounded (%u s)", t.wait4_total);
    CHECK(t.skipped4 >= 80, "2G-only: 4G skipped outside probes (%u)", t.skipped4);

    g_file[0] = '\0';
    t = run("4G registers but data fails", &weak4g, 96);
    CHECK(t.lost <= 2 + 3, "weak 4G: only the first slots and the spaced-out checks lost (%u)", t.lost);
    CHECK(t.sent2 >= 88, "weak 4G: moves to 2G (%u)", t.sent2);

    g_file[0] = '\0';
    t = run("Slow 4G (9 s to register)", &slow4g, 96);
    CHECK(t.sent4 >= 90, "slow 4G: still used, wait learned (%u)", t.sent4);

    /* 4G comes back to a site that has been on 2G */
    g_file[0] = '\0';
    run("2G-only for a day...", &only2g, 96);
    t = run("...then 4G returns", &good4g, 64);
    CHECK(t.sent4 >= 1, "4G found again within the longest check spacing (%u on 4G)", t.sent4);
    CHECK(strncmp(g_file, "P4 ", 3) == 0, "back to 4G primary");

    /* An outage on both must not teach anything */
    g_file[0] = '\0';
    run("Good 4G...", &good4g, 8);
    char before[48]; strcpy(before, g_file);
    run("...then no network at all", &nothing, 8);
    CHECK(strncmp(g_file, "P4 ", 3) == 0, "outage leaves 4G primary");
    t = run("...network back", &good4g, 4);
    CHECK(t.sent4 == 4, "after outage straight back on 4G");

    /* 2G registers but its data fails too, while 4G was blamed: go back to 4G first */
    g_file[0] = '\0';
    const site_t both_bad = { true, 2, false, true, false };
    t = run("Data fails on both modes", &both_bad, 20);
    CHECK(t.wait4_total > 0, "keeps trying 4G when 2G is no better");

    /* A site where 4G is skipped as useless, then the operator switches 2G off */
    g_file[0] = '\0';
    const site_t only2g_then = { false, 0, false, true, true };
    run("2G-only site (4G skipped)...", &only2g_then, 96);
    const site_t no2g_now = { true, 2, true, false, false };
    t = run("...then 2G switched off, 4G there", &no2g_now, 8);
    CHECK(t.sent4 == 8, "2G gone: rescued onto 4G at once (%u of 8)", t.sent4);
    CHECK(t.rescues <= 1, "only the first slot needs the rescue (%u)", t.rescues);
    CHECK(strncmp(g_file, "P4 ", 3) == 0, "4G primary after the rescue");

    /* SIM swap to another operator resets; same operator keeps the learning */
    g_file[0] = '\0';
    g_imsi = "404719180719647";
    run("BSNL SIM on a 2G-only site...", &only2g, 96);
    CHECK(strstr(g_file, "N40471") != NULL, "operator kept in the file");
    g_imsi = "404710000000001";
    t = run("...another BSNL SIM", &only2g, 4);
    CHECK(t.skipped4 >= 3, "same operator: learning kept (4G skipped %u)", t.skipped4);
    g_imsi = "405854000000001";                 /* Jio: 4G only */
    const site_t jio = { true, 1, true, false, false };
    t = run("...Jio SIM (no 2G)", &jio, 8);
    CHECK(t.sent4 == 8 && t.rescues == 0, "new operator: starts afresh on 4G (%u sent, %u rescues)", t.sent4, t.rescues);
    CHECK(strstr(g_file, "N40585") != NULL, "new operator recorded");
    g_imsi = "";
    t = run("...SIM unreadable this time", &jio, 2);
    CHECK(strstr(g_file, "N40585") != NULL && t.sent4 == 2, "unreadable IMSI changes nothing");
    g_imsi = "404719180719647";

    /* Older files */
    net_policy_t p;
    net_policy_parse(&p, "13\r\n");
    CHECK(p.primary == NET_2G, "old 13 file read as 2G primary");
    net_policy_parse(&p, "38\r\n");
    CHECK(p.primary == NET_4G, "old 38 file read as 4G primary");
    net_policy_parse(&p, "");
    CHECK(p.primary == NET_4G && p.reg4_sec == 0, "no file: defaults");
    char text[48];
    net_policy_defaults(&p); p.primary = NET_2G; p.probe_count = 15; p.reg4_sec = 7; p.miss4 = 99; p.datafail4 = 3; p.datafail2 = 1; p.probe_backoff = 2; p.probing = 1;
    int n = net_policy_format(&p, text, sizeof(text));
    net_policy_t q; net_policy_parse(&q, text);
    CHECK(memcmp(&p, &q, sizeof(p)) == 0, "format/parse round trip");
    CHECK(n == NET_POLICY_TEXT_LEN, "fixed length %d", n);
    memcpy(p.op, "40471", 6); n = net_policy_format(&p, text, sizeof(text)); net_policy_parse(&q, text);
    CHECK(n == NET_POLICY_TEXT_LEN && memcmp(&p, &q, sizeof(p)) == 0, "round trip with operator");
    net_policy_parse(&q, "P2 C05 R01 M04 F00 G00\r\n");   /* written before X and L existed */
    CHECK(q.primary == NET_2G && q.probe_backoff == 0 && q.probing == 0 && q.miss4 == 4, "file without X/L fields");

    puts(failures ? "FAILED" : "ALL PASS");
    return failures != 0;
}
