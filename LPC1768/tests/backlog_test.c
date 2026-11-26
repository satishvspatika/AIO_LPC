/* Host test (7.02): the SD backlog, slot after slot, against a pretend server.
 * Runs the firmware's own code, taken from the sources by bench/host_tests.sh:
 *   bk_store.inc  storage_log.c's backlog section (BACKLOG.DAT on a pretend card)
 *   bk_post.inc   main.c post_backlog_record()
 *   bk_loop.inc   main.c's backlog loop of a slot whose own record was delivered
 * Built with -DOLD_LOOP from the 7.01 sources too: shows the stuck backlog the field saw.
 * Each record's payload is "id=<n>"; the server's answer per record is set by the scenario:
 *   O ok, R "Rejected", E answered but not OK (HTTP 500, a "duplicate" page), U sent but no answer,
 *   N no connection, F no connection the first time then OK. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- what the extracted code needs ---- */
typedef uint32_t DWORD;
typedef unsigned int UINT;
typedef uint8_t BYTE;
typedef int FRESULT;
#define FR_OK 0
#define FR_NO_FILE 4
#define FA_READ 0x01
#define FA_WRITE 0x02
#define FA_OPEN_EXISTING 0x00
#define FA_OPEN_ALWAYS 0x10
typedef struct { int dummy; } FATFS;
typedef struct { DWORD fsize; DWORD fptr; int open; } FIL;
typedef unsigned long portTickType;
#define portTICK_RATE_MS 1
#define REJECTED_FILENAME "REJECTED.TXT"
#define REJECTIONS_ALLOWED 3
#define KSNDMC_TRG 0
#define BIHAR_TRG 1
#define KSNDMC_TWS 2
#define UNIT_CFG BIHAR_TRG
#ifndef STORAGE_BACKLOG_ENTRY_SIZE
#define STORAGE_BACKLOG_ENTRY_SIZE 401u
#endif
typedef struct { const char *domain; } http_endpoint_t;

/* pretend card: one file */
static uint8_t s_card[4 * 1024 * 1024];
static DWORD s_card_size = 0;
static bool s_card_exists = false;
static unsigned long s_bytes_read = 0;
static FATFS s_fatfs;
static bool s_fs_mounted = true;
static FIL s_backlog_file;
static void fs_lock(void) {}
static void fs_unlock(void) {}
static FRESULT f_mount(BYTE v, FATFS *f) { (void)v; (void)f; return FR_OK; }
static FRESULT f_open(FIL *f, const char *name, BYTE mode)
{
    (void)name;
    if (!s_card_exists) {
        if (!(mode & FA_OPEN_ALWAYS)) return FR_NO_FILE;
        s_card_exists = true;
        s_card_size = 0;
    }
    f->fsize = s_card_size; f->fptr = 0; f->open = 1;
    return FR_OK;
}
static FRESULT f_read(FIL *f, void *buf, UINT n, UINT *br)
{
    UINT k = (f->fptr + n <= s_card_size) ? n : (s_card_size > f->fptr ? s_card_size - f->fptr : 0);
    memcpy(buf, s_card + f->fptr, k); f->fptr += k; *br = k; s_bytes_read += k;
    return FR_OK;
}
static FRESULT f_write(FIL *f, const void *buf, UINT n, UINT *bw)
{
    memcpy(s_card + f->fptr, buf, n); f->fptr += n;
    if (f->fptr > s_card_size) s_card_size = f->fptr;
    f->fsize = s_card_size; *bw = n;
    return FR_OK;
}
static FRESULT f_lseek(FIL *f, DWORD pos) { f->fptr = pos; return FR_OK; }
static FRESULT f_truncate(FIL *f) { s_card_size = f->fptr; f->fsize = s_card_size; return FR_OK; }
static FRESULT f_close(FIL *f) { f->open = 0; return FR_OK; }
static FRESULT f_unlink(const char *name) { (void)name; s_card_exists = false; s_card_size = 0; return FR_OK; }

static long s_count = 0;   /* the LCD's backlog count */
static void ui_inc_backlog_count(void) { s_count++; }
static void ui_dec_backlog_count(void) { s_count--; }
static void ui_set_backlog_count(uint32_t c) { s_count = (long)c; }
static int s_rejected_file = 0;
static void storage_append_stamped(const char *name, const char *text) { (void)name; (void)text; s_rejected_file++; }
static void storage_log_debug(const char *tag, const char *msg) { (void)tag; (void)msg; }
static void hal_power_heartbeat(void) {}
static portTickType s_now = 0;
static portTickType xTaskGetTickCount(void) { return s_now; }
static void vTaskDelay(portTickType t) { s_now += t; }
static bool tws_payload_th_missing(const char *p) { (void)p; return false; }

/* pretend server */
static char s_beh[2000];            /* behaviour per record id */
static int s_seen_first[2000];
static int s_delivered[2000];
static bool s_rej, s_ans, s_unans;
static bool sim7600_post_rejected(void) { return s_rej; }
static bool sim7600_post_answered(void) { return s_ans; }
static bool sim7600_post_unanswered(void) { return s_unans; }
static bool sim7600_still_registered(void) { return true; }
static bool post_to(const http_endpoint_t *e, const char *payload, int route)
{
    (void)e; (void)route;
    int id = atoi(payload + 3);
    s_now += 8000;                  /* a socket open, send and answer */
    s_rej = s_ans = s_unans = false;
    char b = s_beh[id];
    if (b == 'F') { b = s_seen_first[id]++ ? 'O' : 'N'; }
    switch (b) {
    case 'O': s_ans = true; s_delivered[id]++; return true;
    case 'R': s_ans = true; s_rej = true; return false;
    case 'E': s_ans = true; return false;
    case 'U': s_unans = true; return false;
    default: return false;          /* N */
    }
}

/* the firmware's code */
#define BACKLOG_FILENAME "BACKLOG.DAT"
uint32_t storage_backlog_get_pending_count(void);
#include "bk_store.inc"
#include "bk_post.inc"

static void run_slot(void)
{
    char payload[512];
    static http_endpoint_t ep = { "x" };
    const http_endpoint_t *endpoint = &ep;
    int good_route = 0;
    const portTickType budget_end = xTaskGetTickCount() + 300000;
#define SLOT_BUDGET_LEFT() ((long)(budget_end - xTaskGetTickCount()) > 0)
#include "bk_loop.inc"
#undef SLOT_BUDGET_LEFT
    (void)bk_failed;
    s_now += 900000;
}

static int s_fails = 0;
static void check(const char *what, long got, long want)
{
    int ok = got == want; if (!ok) s_fails++;
    printf("%s %-68s got %5ld want %5ld\n", ok ? "PASS" : "FAIL", what, got, want);
}
static void reset(void)
{
    s_card_exists = false; s_card_size = 0; s_count = 0; s_rejected_file = 0; s_bytes_read = 0;
    memset(s_beh, 'O', sizeof s_beh); memset(s_seen_first, 0, sizeof s_seen_first);
    memset(s_delivered, 0, sizeof s_delivered);
}
static void queue(int from, int to)
{
    char p[32];
    for (int i = from; i <= to; i++) { snprintf(p, sizeof p, "id=%d", i); storage_backlog_save("20261126000000", p); }
}
static long delivered(int from, int to) { long n = 0; for (int i = from; i <= to; i++) n += s_delivered[i] > 0; return n; }
static long dup_sends(int from, int to) { long n = 0; for (int i = from; i <= to; i++) n += s_delivered[i] > 1; return n; }

int main(void)
{
#ifdef OLD_LOOP
    printf("-- 7.01 backlog loop --\n");
#else
    printf("-- 7.02 backlog loop --\n");
#endif
    /* 1: plain backlog of 40 */
    reset(); queue(1, 40);
    run_slot(); check("40 good records: sent in the first slot (max 15)", delivered(1, 40), 15);
    run_slot(); run_slot();
    check("40 good records: all sent after 3 slots", delivered(1, 40), 40);
    check("  none sent twice", dup_sends(1, 40), 0);
    check("  backlog count back to 0", s_count, 0);
    check("  BACKLOG.DAT removed", s_card_exists, 0);

    /* 2: the field case: one record the server answers with an error, 30 good ones behind it */
    reset(); queue(1, 31); s_beh[1] = 'E';
    for (int s = 0; s < 10; s++) run_slot();
    check("error-answered record first, 30 behind: good ones sent in 10 slots", delivered(2, 31), 30);
    check("  the bad one set aside to REJECTED.TXT", s_rejected_file, 1);
    check("  backlog count back to 0", s_count, 0);

    /* 3: one rejected record ("Rejected") first, 30 behind */
    reset(); queue(1, 31); s_beh[1] = 'R';
    run_slot(); run_slot();
    check("rejected record first, 30 behind: good ones sent in 2 slots", delivered(2, 31), 30);
    run_slot();
    check("  rejected one set aside after its 3rd slot", s_rejected_file, 1);

    /* 4: no connection part way: the rest waits, nothing lost, nothing sent twice */
    reset(); queue(1, 20); s_beh[5] = 'F';
    run_slot();
    check("connection fails once at #5: #1-4 sent, then retried at once", delivered(1, 20), 15);
    run_slot();
    check("  all 20 sent next slot", delivered(1, 20), 20);
    check("  none sent twice", dup_sends(1, 20), 0);

    /* 5: sent with no answer: kept for the next slot (the server may have it), nothing set aside */
    reset(); queue(1, 5); s_beh[2] = 'U';
    run_slot();
    check("no answer at #2: stops there this slot", delivered(1, 5), 1);
    check("  not set aside", s_rejected_file, 0);

    /* 6: a long outage: 1500 sent entries at the front of the file, 15 waiting */
    reset(); queue(1, 1500);
    for (DWORD off = 0; off < s_card_size; off += STORAGE_BACKLOG_ENTRY_SIZE) s_card[off] = 1;   /* sent */
    s_count = 0;
    queue(1501, 1515); s_bytes_read = 0;
    run_slot();
    check("1500 sent entries before 15 waiting: all 15 sent in one slot", delivered(1501, 1515), 15);
    printf("     card read for that slot: %lu KB\n", s_bytes_read / 1024);
    check("  card read under 2 MB (one scan, not one per record)", s_bytes_read < 2000000UL, 1);

    printf("%s (%d failed)\n", s_fails ? "FAILED" : "ALL PASSED", s_fails);
    return s_fails != 0;
}
