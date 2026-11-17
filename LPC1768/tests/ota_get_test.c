/* Host test for the modem driver's get_collect() (the reply to an OTA GET, put together from the
 * modem's "+IPD<n>" blocks). The function is taken as it stands from sim7600_driver.c:
 *   sed -n '/^static size_t __attribute__((noinline)) get_collect/,/^const char \*sim7600_http_get/p' \
 *       sim7600_driver.c | sed '$d' > get_collect.inc
 * Bytes arrive as from the UART at 115200 baud (about 115 per 10 ms), into a ring that wraps. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RX_BUFFER_SIZE  2048
static char s_rx_buf[RX_BUFFER_SIZE];
static volatile uint16_t s_rx_head = 0;
static volatile uint16_t s_rx_tail = 0;
static char s_rx_temp[RX_BUFFER_SIZE];
static bool s_get_open = true;
static int s_sms_notes = 0;
static void note_sms_arrival(void) { s_sms_notes++; }

static const char *s_feed;      /* What the modem still has to send */
static size_t s_feed_left;
static size_t s_feed_rate = 115;
static uint32_t s_stall_ms = 0; /* The modem says nothing for this long first */
static bool s_overflowed = false;
static void delay_ms(uint32_t ms)
{
    if (s_stall_ms >= ms) { s_stall_ms -= ms; return; }
    size_t n = s_feed_rate * ms / 10;
    while (n-- && s_feed_left) {
        uint16_t next = (uint16_t)((s_rx_head + 1) % RX_BUFFER_SIZE);
        if (next == s_rx_tail) { s_overflowed = true; break; }
        s_rx_buf[s_rx_head] = *s_feed++;
        s_rx_head = next;
        s_feed_left--;
    }
}

#include "get_collect.inc"

static int s_fails = 0, s_checks = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_fails++; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static char s_stream[8192];
static size_t s_stream_len;
static void put(const char *text, size_t n) { memcpy(s_stream + s_stream_len, text, n); s_stream_len += n; }
static void puts_(const char *text) { put(text, strlen(text)); }

/* The modem's bytes for an HTTP reply cut into +IPD blocks at the given places */
static void modem_stream(const char *http, size_t http_len, const size_t *cuts, int n_cuts, bool sms_between, bool close_after)
{
    s_stream_len = 0;
    puts_("\r\nOK\r\n\r\n+CIPSEND: 0,84,84\r\n");
    size_t at = 0;
    for (int i = 0; i <= n_cuts; i++) {
        size_t end = (i < n_cuts) ? cuts[i] : http_len;
        char head[64];
        if (end <= at) continue;
        snprintf(head, sizeof(head), "\r\nRECV FROM:187.127.175.183:80\r\n+IPD%u\r\n", (unsigned)(end - at));
        puts_(head);
        put(http + at, end - at);
        at = end;
        if (sms_between && i == 0) puts_("\r\n+CMTI: \"SM\",3\r\n");
    }
    if (close_after) puts_("\r\n+IPCLOSE: 0,1\r\n");
}

static size_t run(uint16_t ring_start, uint32_t timeout_ms, int *status, int *body_at)
{
    s_rx_head = s_rx_tail = ring_start;
    s_feed = s_stream; s_feed_left = s_stream_len; s_overflowed = false; s_get_open = true;
    memset(s_rx_temp, 'X', sizeof(s_rx_temp));
    *status = 0;
    return get_collect(timeout_ms, status, body_at);
}

int main(void)
{
    static char http[4096], body[1100];
    int status, body_at;

    /* A piece as the server sends it through nginx: "@crc\n" + 1024 bytes of .hex text */
    srand(7);
    memcpy(body, "@1A2B3C4D\n", 10);
    for (int i = 10; i < 1034; i++) body[i] = ":0123456789ABCDEF\r\n"[rand() % 19];
    /* Text that looks like modem lines, inside the data: must be taken as data */
    memcpy(body + 300, "\r\n+IPD99\r\n+IPCLOSE: 0,1\r\n", 25);
    int head_len = snprintf(http, sizeof(http), "HTTP/1.1 200 OK\r\nServer: nginx/1.28.0\r\nDate: Tue, 17 Nov 2026 08:00:00 GMT\r\n"
                            "Content-Type: application/octet-stream\r\nContent-Length: 1034\r\nConnection: keep-alive\r\n\r\n");
    memcpy(http + head_len, body, 1034);
    const size_t http_len = (size_t)head_len + 1034;

    /* One block; then cut in two at every position (through the status line, the headers, the blank
     * line and the data), with the ring starting anywhere, an SMS notice between, a close after */
    for (size_t cut = 0; cut <= http_len; cut += (cut < (size_t)head_len + 30 ? 1 : 37)) {
        const size_t cuts[1] = { cut };
        modem_stream(http, http_len, cuts, 1, (cut % 3) == 0, (cut % 5) == 0);
        s_sms_notes = 0;
        size_t total = run((uint16_t)((cut * 131) % RX_BUFFER_SIZE), 15000, &status, &body_at);
        CHECK(!s_overflowed);
        CHECK(total == http_len && status == 200 && body_at == head_len);
        CHECK(total == http_len && !memcmp(s_rx_temp + body_at, body, 1034));
        CHECK(s_sms_notes == (((cut % 3) == 0 && cut > 0) ? 1 : 0));
    }
    /* Three blocks */
    {
        const size_t cuts[2] = { 100, 700 };
        modem_stream(http, http_len, cuts, 2, true, false);
        size_t total = run(2000, 15000, &status, &body_at);
        CHECK(total == http_len && !memcmp(s_rx_temp + body_at, body, 1034) && s_get_open);
    }
    /* A slow link: the blocks come 3 s apart */
    {
        const size_t cuts[1] = { 500 };
        modem_stream(http, http_len, cuts, 1, false, false);
        s_stall_ms = 3000; s_feed_rate = 20;
        size_t total = run(0, 15000, &status, &body_at);
        CHECK(total == http_len && !memcmp(s_rx_temp + body_at, body, 1034));
        s_feed_rate = 115;
    }
    /* The connection closes with the reply whole: taken, and the socket known closed for the next one */
    {
        modem_stream(http, http_len, NULL, 0, false, true);
        s_feed_rate = 4000;     /* All at once: the close is seen in the same pass */
        size_t total = run(0, 15000, &status, &body_at);
        CHECK(total == http_len && !s_get_open);
        s_feed_rate = 115;
    }
    /* Cut short by a close: nothing */
    {
        const size_t cuts[1] = { 600 };
        modem_stream(http, 600, cuts, 1, false, true);
        size_t total = run(0, 15000, &status, &body_at);
        CHECK(total == 0 && !s_get_open);
    }
    /* Cut short and silent: nothing, after the wait */
    {
        modem_stream(http, 600, NULL, 0, false, false);
        size_t total = run(0, 2000, &status, &body_at);
        CHECK(total == 0);
    }
    /* Nothing at all, or only an error */
    s_stream_len = 0; CHECK(run(0, 500, &status, &body_at) == 0);
    s_stream_len = 0; puts_("\r\nERROR\r\n"); CHECK(run(0, 500, &status, &body_at) == 0);
    /* The short answers */
    {
        int n = snprintf(http, sizeof(http), "HTTP/1.1 200 OK\r\nServer: nginx\r\nContent-Type: text/plain\r\nContent-Length: 32\r\n\r\nOTA FW 340936 E80A91A5 17506204\n");
        modem_stream(http, (size_t)n, NULL, 0, false, false);
        size_t total = run(77, 15000, &status, &body_at);
        CHECK(total == (size_t)n && status == 200 && !strncmp(s_rx_temp + body_at, "OTA FW 340936 E80A91A5 17506204", 31));
        n = snprintf(http, sizeof(http), "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nOTA GONE\n");
        modem_stream(http, (size_t)n, NULL, 0, false, false);
        total = run(77, 15000, &status, &body_at);
        CHECK(total == (size_t)n && status == 404 && !strncmp(s_rx_temp + body_at, "OTA GONE", 8));
        /* No length given (not our server): refused */
        n = snprintf(http, sizeof(http), "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n");
        modem_stream(http, (size_t)n, NULL, 0, false, false);
        CHECK(run(0, 2000, &status, &body_at) == 0);
        /* "Content-Length: " in the body only: not taken for the header */
        n = snprintf(http, sizeof(http), "HTTP/1.1 200 OK\r\nX: y\r\n\r\nContent-Length: 5\r\n");
        modem_stream(http, (size_t)n, NULL, 0, false, false);
        CHECK(run(0, 2000, &status, &body_at) == 0);
        /* A reply longer than the buffer: never written past it */
        n = snprintf(http, sizeof(http), "HTTP/1.1 200 OK\r\nContent-Length: 3000\r\n\r\n");
        memset(http + n, 'A', 3000);
        const size_t cuts[2] = { 1400, 2800 };
        modem_stream(http, (size_t)n + 3000, cuts, 2, false, false);
        (void)run(0, 3000, &status, &body_at);
        CHECK(s_rx_temp[RX_BUFFER_SIZE - 1] == '\0' || s_rx_temp[RX_BUFFER_SIZE - 1] == 'X');
    }
    printf("%s: ota_get_test, %d checks, %d failed\n", s_fails ? "FAILED" : "ALL PASS", s_checks, s_fails);
    return s_fails ? 1 : 0;
}
