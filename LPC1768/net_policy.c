#include "net_policy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t inc(uint8_t v)
{
    return (v < 99) ? (uint8_t)(v + 1) : v;
}

void net_policy_defaults(net_policy_t *p)
{
    memset(p, 0, sizeof(*p));
    p->primary = NET_4G;
}

int net_policy_format(const net_policy_t *p, char *out, size_t size)
{
    return snprintf(out, size, "P%u C%02u R%02u M%02u F%02u G%02u X%u L%u N%-5.5s\r\n", (unsigned)p->primary,
                    (unsigned)p->probe_count % 100, (unsigned)p->reg4_sec % 100, (unsigned)p->miss4 % 100,
                    (unsigned)p->datafail4 % 100, (unsigned)p->datafail2 % 100,
                    (unsigned)p->probe_backoff % 10, (unsigned)p->probing % 10, p->op[0] ? p->op : "-----");
}

static uint8_t field(const char *text, char key)
{
    char tag[3] = { ' ', key, '\0' };
    const char *at = strstr(text, tag);
    return at ? (uint8_t)(atoi(at + 2) % 100) : 0;
}

void net_policy_parse(net_policy_t *p, const char *text)
{
    net_policy_defaults(p);
    if (!text) {
        return;
    }
    if (text[0] == 'P') {
        p->primary = (text[1] == '2') ? NET_2G : NET_4G;
        p->probe_backoff = field(text, 'X');
        if (p->probe_backoff > NET_PROBE_BACKOFF_MAX) {
            p->probe_backoff = NET_PROBE_BACKOFF_MAX;
        }
        p->probe_count = field(text, 'C') % (NET_PROBE_EVERY << p->probe_backoff);
        p->probing = field(text, 'L') ? 1 : 0;
        const char *op = strstr(text, " N");
        if (op && op[2] >= '0' && op[2] <= '9') {
            memcpy(p->op, op + 2, 5);
            p->op[5] = '\0';
        }
        p->reg4_sec = field(text, 'R');
        p->miss4 = field(text, 'M');
        p->datafail4 = field(text, 'F');
        p->datafail2 = field(text, 'G');
    } else if (atoi(text) == 13) {
        /* v6.9.16 test builds and older: "13" or "13,count" meant 2G last worked */
        const char *count = strchr(text, ',');
        p->primary = NET_2G;
        p->probe_count = count ? (uint8_t)(atoi(count + 1) % NET_PROBE_EVERY) : 0;
    }
}

bool net_policy_set_operator(net_policy_t *p, const char *imsi)
{
    if (!imsi || strlen(imsi) < 5) {
        return false;
    }
    for (int i = 0; i < 5; i++) {
        if (imsi[i] < '0' || imsi[i] > '9') {
            return false;
        }
    }
    bool changed = p->op[0] != '\0' && strncmp(p->op, imsi, 5) != 0;
    if (changed) {
        net_policy_defaults(p);
    }
    memcpy(p->op, imsi, 5);
    p->op[5] = '\0';
    return changed;
}

net_plan_t net_policy_plan(net_policy_t *p)
{
    net_plan_t plan = { true, NET_4G_WAIT_UNKNOWN_SEC, false };
    p->probing = 0;

    if (p->primary == NET_4G) {
        p->probe_count = 0;
        if (p->reg4_sec != 0) {
            /* Twice what it usually takes, plus a margin: a cell that answers in 1 s gets 6 s */
            unsigned w = 2u * p->reg4_sec + 4u;
            plan.wait_4g_sec = (uint8_t)(w < NET_4G_WAIT_MIN_SEC ? NET_4G_WAIT_MIN_SEC : (w > NET_4G_WAIT_MAX_SEC ? NET_4G_WAIT_MAX_SEC : w));
        }
        return plan;
    }

    /* 2G is primary: 4G still goes first, briefly, and in full every 16 power-ons (32, 64 while those
     * full checks keep failing, so a site with no usable 4G stops paying for them) */
    p->probe_count = (uint8_t)((p->probe_count + 1) % (NET_PROBE_EVERY << p->probe_backoff));
    if (p->probe_count == 0) {
        p->probing = 1;
        plan.full_probe = true;
        plan.wait_4g_sec = NET_4G_WAIT_MAX_SEC;
        return plan;
    }
    if (p->miss4 >= NET_SKIP_4G_AFTER_MISS || p->datafail4 >= NET_SKIP_4G_AFTER_FAIL) {
        plan.search_4g = false;      /* 4G is no use here for now: not even the short search */
        plan.wait_4g_sec = 0;
        return plan;
    }
    plan.wait_4g_sec = NET_4G_WAIT_AFTER_2G;
    return plan;
}

void net_policy_registered(net_policy_t *p, bool searched_4g, int mode, uint8_t sec_to_4g)
{
    if (mode == NET_4G) {
        p->miss4 = 0;
        if (sec_to_4g == 0) {
            sec_to_4g = 1;
        }
        /* Running average, 3 parts old to 1 part new, so one slow search doesn't stretch every wait */
        p->reg4_sec = (p->reg4_sec == 0) ? sec_to_4g : (uint8_t)((3u * p->reg4_sec + sec_to_4g + 2u) / 4u);
    } else if (mode == NET_2G && searched_4g) {
        p->miss4 = inc(p->miss4);
        if (p->probing) {
            p->probing = 0;
            if (p->probe_backoff < NET_PROBE_BACKOFF_MAX) {
                p->probe_backoff++;
            }
        }
    }
    /* No registration on either: a network outage, which says nothing about which mode is better */
}

void net_policy_delivery(net_policy_t *p, int mode, bool delivered, bool network_fault)
{
    if (mode != NET_4G && mode != NET_2G) {
        return;
    }
    if (mode == NET_4G && p->probing) {
        /* A full 4G check that found 4G: spacing back to normal if it delivered, wider if not */
        p->probing = 0;
        if (delivered) {
            p->probe_backoff = 0;
        } else if (network_fault && p->probe_backoff < NET_PROBE_BACKOFF_MAX) {
            p->probe_backoff++;
        }
    }
    if (delivered) {
        p->primary = (uint8_t)mode;
        if (mode == NET_4G) {
            p->datafail4 = 0;
            p->miss4 = 0;
            p->probe_backoff = 0;
        } else {
            p->datafail2 = 0;
        }
        return;
    }
    if (!network_fault) {
        return;   /* A server refusal is no reason to change radio mode */
    }
    if (mode == NET_4G) {
        p->datafail4 = inc(p->datafail4);
        if (p->datafail4 >= NET_SKIP_4G_AFTER_FAIL) {
            p->primary = NET_2G;     /* Registers on 4G but can't send: the next slots try 2G */
        }
    } else {
        p->datafail2 = inc(p->datafail2);
        if (p->datafail2 >= NET_SKIP_4G_AFTER_FAIL && p->primary == NET_2G) {
            p->primary = NET_4G;     /* 2G no better: back to 4G first */
            p->datafail4 = 0;
            p->miss4 = 0;
        }
    }
}
