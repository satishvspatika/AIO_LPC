#ifndef NET_POLICY_H
#define NET_POLICY_H

/* Which radio mode to search first, and for how long, learned from what worked at this site.
 * No hardware here: the modem driver asks for a plan before registering and reports what happened,
 * so every case can be tested on the host (LPC1768/tests/net_policy_test.c). */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define NET_4G 4
#define NET_2G 2

typedef struct {
    uint8_t primary;       /* NET_4G or NET_2G: the mode that last delivered a slot */
    uint8_t probe_count;   /* Power-ons since 4G last had a full search while 2G is primary */
    uint8_t reg4_sec;      /* Learned 4G registration time in seconds (running average), 0 = not known */
    uint8_t miss4;         /* Power-ons in a row where 4G was searched and 2G registered instead */
    uint8_t datafail4;     /* Slots in a row registered on 4G whose data still failed */
    uint8_t datafail2;     /* The same on 2G */
    uint8_t probe_backoff; /* Full 4G checks that failed in a row (0-2): the check spaces out 16, 32, 64 */
    uint8_t probing;       /* 1 while this power-on is a full 4G check, until its outcome is known */
    char op[6];            /* SIM operator (IMSI MCC+MNC) this was learned with, "" = not known */
} net_policy_t;

typedef struct {
    bool search_4g;        /* False: straight to 2G (4G is known to be no use here) */
    uint8_t wait_4g_sec;   /* 4G search time before switching to 2G */
    bool full_probe;       /* This power-on gives 4G its full search again */
} net_plan_t;

#define NET_4G_WAIT_MIN_SEC     6   /* About 2 polls: the shortest that still finds a 4G cell answering at once */
#define NET_4G_WAIT_MAX_SEC     20
#define NET_4G_WAIT_UNKNOWN_SEC 16  /* No history yet: about the old 5-poll wait */
#define NET_4G_WAIT_AFTER_2G    6
#define NET_PROBE_EVERY         16  /* About every 4 hours of slots; 8 and 16 hours after failed checks */
#define NET_PROBE_BACKOFF_MAX   2
#define NET_SKIP_4G_AFTER_MISS  4   /* 4G not found this many times running: don't search it outside probes */
#define NET_SKIP_4G_AFTER_FAIL  2   /* Registered on 4G but data failed this many slots running */
#define NET_2G_ONLY_WAIT_SEC    15  /* 4G skipped but no 2G after this long: try 4G in the same power-on */
#define NET_POLICY_TEXT_LEN     37

void net_policy_defaults(net_policy_t *p);
/* Fixed-length text for RATMODE.DAT (overwritten in place); parse accepts the older "13" / "38,00"
 * files too. Returns the text length. */
int net_policy_format(const net_policy_t *p, char *out, size_t size);
void net_policy_parse(net_policy_t *p, const char *text);

/* What was learned holds for one operator at one site: a SIM from another operator (a Jio SIM has no
 * 2G at all) starts afresh. Another SIM from the same operator keeps it. imsi may be "" (unreadable),
 * which changes nothing. Returns true if the learning was reset. */
bool net_policy_set_operator(net_policy_t *p, const char *imsi);

/* Called once per modem power-on, before searching. Moves the probe count on. */
net_plan_t net_policy_plan(net_policy_t *p);
/* After the search: the mode that registered (NET_4G, NET_2G, or 0 for none) and how long the 4G
 * search took when 4G registered. searched_4g says whether this power-on looked for 4G at all. */
void net_policy_registered(net_policy_t *p, bool searched_4g, int mode, uint8_t sec_to_4g);
/* After a slot's send, on the mode it registered on. network_fault: not sent because the network
 * failed (not a server refusal). */
void net_policy_delivery(net_policy_t *p, int mode, bool delivered, bool network_fault);

#endif
