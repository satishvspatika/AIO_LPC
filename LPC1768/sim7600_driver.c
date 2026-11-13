#include "sim7600_driver.h"
#include "hal_power.h"
#include "storage_log.h"
#include "user_config.h"
#include "flash_nvram.h"
#include "flash_log.h"
#include "intermcu_protocol.h"
#include "telemetry_formatter.h"
#include "lpc17xx_uart.h"
#include "lpc17xx_pinsel.h"
#include "lpc17xx_gpio.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "net_policy.h"

#define NET_VOICE_ONLY_WAIT_SEC 8   /* CREG registered: this long for data registration before going on */

static net_policy_t s_net;      /* Radio mode learning, kept in RATMODE.DAT between power-ons */
static int s_net_mode = 0;      /* Mode this power-on registered on: NET_4G, NET_2G or 0 */

static void save_net_policy(void)
{
    char text[NET_POLICY_TEXT_LEN + 8];
    net_policy_format(&s_net, text, sizeof(text));
    storage_state_save(RAT_MODE_FILENAME, text);
}

void sim7600_note_slot_delivery(bool delivered, bool network_fault)
{
    if (s_net_mode == 0) {
        return;
    }
    net_policy_t before = s_net;
    net_policy_delivery(&s_net, s_net_mode, delivered, network_fault);
    if (memcmp(&before, &s_net, sizeof(s_net)) != 0) {
        save_net_policy();
        if (before.primary != s_net.primary) {
            storage_log_debug("MODEM", s_net.primary == NET_2G ? "4G registers but can't send here: 2G first from now"
                                                               : "Back to 4G first");
        }
    }
}

#define UART_MODEM      LPC_UART2
#define PIN_SIM_SEL     0       /* P0.0: SIM select (Output LOW = Holder J1A) */
#define PIN_PWR_KEY     1       /* P0.1: SIM7600 PWRKEY (Active LOW pulse, resting HIGH) */

#define RX_BUFFER_SIZE  2048
static char s_rx_buf[RX_BUFFER_SIZE];
static volatile uint16_t s_rx_head = 0;
static volatile uint16_t s_rx_tail = 0;

/* Static buffers to prevent task stack overflows in FreeRTOS */
static char s_rx_temp[RX_BUFFER_SIZE];
static char s_http_req[1024];
static char s_http_capture[1024];
static const char *s_active_apn = "";
static bool s_bearer_ready = false; /* AT+CGACT done since the modem last started or changed APN */
static char s_sms_capture[512];

static modem_status_t s_modem_status = {
    .csq_val = 99,
    .csq_dbm = -113,
    .ip_addr = "0.0.0.0",
    .iccid = "",
    .carrier = "SEARCHING",
    .is_registered = false,
    .is_pdp_active = false
};

static volatile bool s_modem_busy = false;

bool sim7600_acquire(uint32_t timeout_ms)
{
    uint32_t elapsed = 0;
    while (s_modem_busy && elapsed < timeout_ms) {
        vTaskDelay((50 + portTICK_RATE_MS - 1) / portTICK_RATE_MS);
        elapsed += 50;
    }
    if (!s_modem_busy) {
        s_modem_busy = true;
        return true;
    }
    return false;
}

void sim7600_release(void)
{
    s_modem_busy = false;
}

bool sim7600_is_busy(void)
{
    return s_modem_busy;
}

static void delay_ms(uint32_t ms)
{
    vTaskDelay((ms + portTICK_RATE_MS - 1) / portTICK_RATE_MS);
}

static sim7600_progress_cb_t s_progress_cb = NULL;
static volatile bool s_cancel_requested = false;

void sim7600_set_progress_callback(sim7600_progress_cb_t cb)
{
    s_progress_cb = cb;
}

void sim7600_request_cancel(void)
{
    s_cancel_requested = true;
}

void sim7600_clear_cancel(void)
{
    s_cancel_requested = false;
}

bool sim7600_is_cancel_requested(void)
{
    return s_cancel_requested;
}

static void report_progress(const char *msg)
{
    if (msg) {
        storage_log_debug("STEP", msg);
    }
    if (s_progress_cb && msg) {
        s_progress_cb(msg);
    }
}

void UART2_IRQHandler(void)
{
    uint32_t int_id = (UART_MODEM->IIR & UART_IIR_INTID_MASK);
    if (int_id == UART_IIR_INTID_RDA || int_id == UART_IIR_INTID_CTI) {
        while (LPC_UART2->LSR & UART_LSR_RDR) {
            uint8_t c = (uint8_t)LPC_UART2->RBR;
            uint16_t next = (s_rx_head + 1) % RX_BUFFER_SIZE;
            if (next != s_rx_tail) {
                s_rx_buf[s_rx_head] = (char)c;
                s_rx_head = next;
            }
        }
    }
}

void sim7600_init(void)
{
    PINSEL_CFG_Type pin_cfg;

    /* P0.10: TXD2, P0.11: RXD2 */
    pin_cfg.Portnum = 0;
    pin_cfg.Funcnum = 1;
    pin_cfg.Pinmode = 0;
    pin_cfg.OpenDrain = 0;
    pin_cfg.Pinnum = 10;
    PINSEL_ConfigPin(&pin_cfg);
    pin_cfg.Pinnum = 11;
    PINSEL_ConfigPin(&pin_cfg);

    /* P0.0: SIM_SEL (GPIO output driven LOW = SIM holder J1A) */
    pin_cfg.Funcnum = 0;
    pin_cfg.Pinmode = PINSEL_PINMODE_PULLUP;
    pin_cfg.Pinnum = PIN_SIM_SEL;
    PINSEL_ConfigPin(&pin_cfg);
    LPC_GPIO0->FIOCLR = (1UL << PIN_SIM_SEL);   /* LOW = Holder J1A */
    LPC_GPIO0->FIODIR |= (1UL << PIN_SIM_SEL);  /* Output */

    /* P0.1: SIM7600 PWRKEY (GPIO output resting HIGH, active LOW pulse) */
    pin_cfg.Pinnum = PIN_PWR_KEY;
    PINSEL_ConfigPin(&pin_cfg);
    LPC_GPIO0->FIOSET = (1UL << PIN_PWR_KEY); /* Default resting HIGH */
    LPC_GPIO0->FIODIR |= (1UL << PIN_PWR_KEY);/* Output */

    /* Immediately request LPC845 sentry to activate 3.8V switching regulator on boot */
    hal_power_modem_on();

    UART_CFG_Type uart_cfg;
    UART_ConfigStructInit(&uart_cfg);
    uart_cfg.Baud_rate = 115200;
    UART_Init(UART_MODEM, &uart_cfg);

    UART_FIFO_CFG_Type fifo_cfg;
    UART_FIFOConfigStructInit(&fifo_cfg);
    UART_FIFOConfig(UART_MODEM, &fifo_cfg);

    UART_IntConfig(UART_MODEM, UART_INTCFG_RBR, ENABLE);
    NVIC_SetPriority(UART2_IRQn, 6); /* GPRS_V2_UART2_IRQ_PRIO >= configMAX_SYSCALL */
    NVIC_EnableIRQ(UART2_IRQn);
    UART_TxCmd(UART_MODEM, ENABLE);
}

/* Routine commands whose replies tell nothing (an empty SMS list every few seconds, closing a socket
 * that is already closed) are kept out of the log, so it shows what matters */
static bool s_log_quiet = false;

static void __attribute__((noinline)) send_raw(const char *cmd)
{
    if (!s_log_quiet) {
        storage_log_debug("MODEM_TX", cmd);
    }
    UART_Send(UART_MODEM, (uint8_t *)cmd, strlen(cmd), BLOCKING);
}

static void clear_rx(void)
{
    NVIC_DisableIRQ(UART2_IRQn);
    s_rx_head = 0;
    s_rx_tail = 0;
    NVIC_EnableIRQ(UART2_IRQn);
}

/* A match can land mid-reply ("+NETOPEN:" before its ": 1"). Wait for the modem to stop sending,
 * then take everything, so nothing acts on half a line. */
static void rx_settle_and_snapshot(void)
{
    uint16_t last = s_rx_head;
    for (int i = 0; i < 15; i++) {
        delay_ms(20);
        if (s_rx_head == last) {
            break;
        }
        last = s_rx_head;
    }

    uint16_t tail = s_rx_tail;
    uint16_t head = s_rx_head;
    uint16_t idx = 0;
    while (tail != head && idx < sizeof(s_rx_temp) - 1) {
        s_rx_temp[idx++] = s_rx_buf[tail];
        tail = (tail + 1) % RX_BUFFER_SIZE;
    }
    s_rx_temp[idx] = '\0';
}

static bool wait_response(const char *expected, uint32_t timeout_ms, char *out_capture, size_t capture_size)
{
    uint32_t elapsed = 0;
    while (elapsed < timeout_ms) {
        if (s_rx_tail != s_rx_head) {
            /* Snapshot current buffer content using static buffer (prevents stack overflow) */
            uint16_t tail = s_rx_tail;
            uint16_t head = s_rx_head;
            uint16_t idx = 0;

            while (tail != head && idx < sizeof(s_rx_temp) - 1) {
                s_rx_temp[idx++] = s_rx_buf[tail];
                tail = (tail + 1) % RX_BUFFER_SIZE;
            }
            s_rx_temp[idx] = '\0';

            char *match = strstr(s_rx_temp, expected);
            if (match != NULL) {
                rx_settle_and_snapshot();
                head = s_rx_head;
                if (out_capture && capture_size > 0) {
                    strncpy(out_capture, s_rx_temp, capture_size - 1);
                    out_capture[capture_size - 1] = '\0';
                }
                if (!s_log_quiet) {
                    storage_log_debug("MODEM_RX", s_rx_temp);
                }
                s_rx_tail = head;
                return true;
            }

            /* Detect error response to avoid useless long timeouts */
            if (strstr(s_rx_temp, "ERROR") != NULL && strstr(expected, "ERROR") == NULL) {
                rx_settle_and_snapshot();
                head = s_rx_head;
                if (out_capture && capture_size > 0) {
                    strncpy(out_capture, s_rx_temp, capture_size - 1);
                    out_capture[capture_size - 1] = '\0';
                }
                if (!s_log_quiet) {
                    storage_log_debug("MODEM_ERR", s_rx_temp);
                }
                s_rx_tail = head;
                return false;
            }
        }
        delay_ms(10);
        elapsed += 10;
    }

    /* Silence is the fault worth seeing: record what was awaited and anything partial */
    char timeout_msg[112];
    uint16_t tail = s_rx_tail, head = s_rx_head, idx = 0;
    while (tail != head && idx < sizeof(s_rx_temp) - 1) {
        s_rx_temp[idx++] = s_rx_buf[tail];
        tail = (tail + 1) % RX_BUFFER_SIZE;
    }
    s_rx_temp[idx] = '\0';
    snprintf(timeout_msg, sizeof(timeout_msg), "No '%s' in %ums. Got: %.60s", expected, (unsigned int)timeout_ms, s_rx_temp);
    if (!s_log_quiet) {
        storage_log_debug("MODEM_TIMEOUT", timeout_msg);
    }
    return false;
}

/* Socket 0 is closed before every connect and after every send, open or not: "+CIPCLOSE: 0,4 ERROR"
 * (not open) is the usual answer and means nothing */
static void close_socket_quiet(uint32_t wait_ms)
{
    s_log_quiet = true;
    send_raw("AT+CIPCLOSE=0\r\n");
    if (wait_ms > 0) {
        wait_response("OK", wait_ms, NULL, 0);
    }
    s_log_quiet = false;
}

static void parse_cops_operator(const char *cops_resp, char *out_carrier, size_t max_len)
{
    if (!cops_resp || !out_carrier || max_len == 0) return;

    const char *q1 = strchr(cops_resp, '\"');
    if (!q1) return;
    const char *q2 = strchr(q1 + 1, '\"');
    if (!q2) return;

    size_t len = q2 - (q1 + 1);
    if (len >= max_len) len = max_len - 1;

    char raw[32];
    strncpy(raw, q1 + 1, len);
    raw[len] = '\0';

    if (strcmp(raw, "40445") == 0 || strcmp(raw, "40552") == 0) {
        strncpy(out_carrier, "Airtel", max_len - 1);
    } else if (strcmp(raw, "40471") == 0 || strcmp(raw, "40475") == 0) {
        strncpy(out_carrier, "BSNL", max_len - 1);
    } else if (strcmp(raw, "40586") == 0 || strcmp(raw, "40587") == 0) {
        strncpy(out_carrier, "Jio", max_len - 1);
    } else if (strcmp(raw, "40411") == 0 || strcmp(raw, "40420") == 0) {
        strncpy(out_carrier, "Vi", max_len - 1);
    } else if (strncasecmp(raw, "airtel", 6) == 0) {
        strncpy(out_carrier, "Airtel", max_len - 1);
    } else if (strncasecmp(raw, "bsnl", 4) == 0 || strncasecmp(raw, "cellone", 7) == 0) {
        strncpy(out_carrier, "BSNL", max_len - 1);
    } else if (strncasecmp(raw, "jio", 3) == 0) {
        strncpy(out_carrier, "Jio", max_len - 1);
    } else if (strncasecmp(raw, "vodafone", 8) == 0 || strncasecmp(raw, "vi", 2) == 0 || strncasecmp(raw, "idea", 4) == 0) {
        strncpy(out_carrier, "Vi", max_len - 1);
    } else {
        strncpy(out_carrier, raw, max_len - 1);
    }
    out_carrier[max_len - 1] = '\0';
}

/* ESP32 flow: the server's clock is the reference. Correct the RTC only past the drift limit:
 * 90s after a delivered record, 45s after a rejected one (often caused by the clock itself). */
#define CLOCK_DRIFT_LIMIT_SEC           90
#define CLOCK_DRIFT_LIMIT_REJECTED_SEC  45
/* CTRL v1.11 keeping the time itself (its RTC chip failed) runs about 0.6% fast, 6 s a slot (001903,
 * 2026-11-13): at 90 s its slots were up to 1.5 minutes early. 15 s stays clear of the server time's
 * 1 s steps and the send delay, and is corrected about every third slot. Corrections come about a
 * minute after a boundary, so the step back never crosses one. */
#define CLOCK_DRIFT_LIMIT_SOFT_CLOCK_SEC 15

static long clock_drift_limit(bool rejected)
{
    if (intermcu_845_rtc_state() > 0) {
        return CLOCK_DRIFT_LIMIT_SOFT_CLOCK_SEC;
    }
    return rejected ? CLOCK_DRIFT_LIMIT_REJECTED_SEC : CLOCK_DRIFT_LIMIT_SEC;
}

static void sync_clock_from_server(const char *http_response, bool rejected)
{
    char server_ts[16];
    telemetry_data_t t;
    char msg[72];

    if (!intermcu_parse_server_time(http_response, server_ts, sizeof(server_ts))) {
        storage_log_debug("RTC", "No usable time in server reply");
        return;
    }

    bool have_clock = intermcu_get_telemetry(&t);
    long drift = have_clock ? intermcu_clock_drift_sec(t.present_time, server_ts) : -1;
    if (drift >= 0 && drift < clock_drift_limit(rejected)) {
        /* Every slot, so a card shows how fast this RTC really drifts */
        snprintf(msg, sizeof(msg), "Clock %lds from server, no change", drift);
        storage_log_debug("RTC", msg);
        return;
    }

    snprintf(msg, sizeof(msg), "RTC corrected from server: %.14s -> %s (drift %lds)",
             have_clock ? t.present_time : "none", server_ts, drift);
    storage_log_debug("RTC", msg);
    hal_power_sync_rtc(server_ts);
    intermcu_update_time(server_ts);
}

/* True if the LPC845's RTC is giving a believable time */
static bool clock_is_valid(void)
{
    telemetry_data_t t;
    return intermcu_get_telemetry(&t) && intermcu_clock_drift_sec(t.present_time, t.present_time) == 0;
}

/* ESP32 flow: the modem's clock may not have heard from the network, so it only sets the RTC
 * when the RTC has no valid time at all */
static bool sim7600_sync_time_step(void)
{
    if (clock_is_valid()) {
        return true;
    }

    char capture[128];
    clear_rx();
    send_raw("AT+CCLK?\r\n");
    if (wait_response("+CCLK:", 2000, capture, sizeof(capture))) {
        char *p = strstr(capture, "+CCLK: \"");
        if (p) {
            int yr = 0, mo = 0, da = 0, hr = 0, mi = 0, se = 0;
            if (sscanf(p + 8, "%d/%d/%d,%d:%d:%d", &yr, &mo, &da, &hr, &mi, &se) >= 5) {
                if (yr < 100) yr += 2000;
                if (da >= 1 && da <= 31 && mo >= 1 && mo <= 12 && yr >= 2024 && yr <= 2079) {
                    char ts14[16];
                    snprintf(ts14, sizeof(ts14), "%04d%02d%02d%02d%02d%02d",
                             yr, mo, da, hr, mi, (se >= 0 && se <= 59) ? se : 0);
                    hal_power_sync_rtc(ts14);
                    intermcu_update_time(ts14);
                    storage_log_debug("RTC", "DS1338 RTC synced from cellular tower time");
                    return true;
                }
            }
        }
    }
    return false;
}

static void sim7600_ntp_sync_step(void)
{
    send_raw("AT+CNTP=\"time.google.com\",22\r\n");
    wait_response("OK", 2000, NULL, 0);

    clear_rx();
    send_raw("AT+CNTP\r\n");
    char capture[64];
    if (wait_response("+CNTP: 0", 8000, capture, sizeof(capture))) {
        storage_log_debug("NTP", "NTP network time synchronized successfully");
        sim7600_sync_time_step();
    }
}

/* APN from the SIM (ESP32 get_network rules). An unknown SIM gets airtelgprs.com. */
/* The SIM's operator, from its ICCID/IMSI: data recovery tries only that operator's APNs */
typedef enum { APN_OP_UNKNOWN = 0, APN_OP_BSNL, APN_OP_AIRTEL, APN_OP_JIO, APN_OP_VI } apn_op_t;
static apn_op_t s_apn_op = APN_OP_UNKNOWN;

static const char *select_apn(const char *iccid, const char *imsi)
{
    static const char *const bsnl_iccid[] = {"89917", "89913", "89915"};
    static const char *const bsnl_imsi[] = {"40434", "40438", "40454", "40455", "40471", "40475", "40522", "40523", "40524"};
    const char *carrier = "";
    const char *apn = "airtelgprs.com";
    bool bsnl = false;

    for (size_t i = 0; i < sizeof(bsnl_iccid) / sizeof(bsnl_iccid[0]); i++) {
        bsnl = bsnl || (strncmp(iccid, bsnl_iccid[i], 5) == 0);
    }
    for (size_t i = 0; i < sizeof(bsnl_imsi) / sizeof(bsnl_imsi[0]); i++) {
        bsnl = bsnl || (strncmp(imsi, bsnl_imsi[i], 5) == 0);
    }

    s_apn_op = APN_OP_UNKNOWN;
    if (bsnl) {
        apn = "bsnlnet";
        carrier = "BSNL";
        s_apn_op = APN_OP_BSNL;
    } else if (strncmp(iccid, "899116", 6) == 0 || strncmp(iccid, "899110", 6) == 0 || strncmp(iccid, "899145", 6) == 0) {
        /* Airtel IoT/M2M SIMs */
        apn = "airteliot.com";
        carrier = "Airtel";
        s_apn_op = APN_OP_AIRTEL;
    } else if (strncmp(imsi, "40586", 5) == 0 || strncmp(imsi, "40587", 5) == 0) {
        apn = "jionet";
        carrier = "Jio";
        s_apn_op = APN_OP_JIO;
    } else if (strncmp(imsi, "40411", 5) == 0 || strncmp(imsi, "40420", 5) == 0) {
        apn = "www";
        carrier = "Vi";
        s_apn_op = APN_OP_VI;
    } else if (strncmp(imsi, "40445", 5) == 0 || strncmp(imsi, "40552", 5) == 0) {
        carrier = "Airtel";
        s_apn_op = APN_OP_AIRTEL;
    }

    if (carrier[0]) {
        strncpy(s_modem_status.carrier, carrier, sizeof(s_modem_status.carrier) - 1);
    }
    return apn;
}

static bool s_sim_missing = false;

bool sim7600_sim_missing(void)
{
    return s_sim_missing;
}

bool sim7600_power_on(void)
{
    if (s_modem_status.is_registered) {
        return true;
    }

    report_progress("MODEM POWER ON ");
    s_net_mode = 0;   /* Set again only if this power-on registers */

    /* 1. Turn ON 3.8V rail via LPC845 sentry command ($n2;) */
    hal_power_modem_on();

    /* 2. Select SIM holder J1A (P0.0 LOW) */
    LPC_GPIO0->FIOCLR = (1UL << PIN_SIM_SEL);       /* Driven LOW = Holder J1A */
    LPC_GPIO0->FIODIR |= (1UL << PIN_SIM_SEL);

    /* Ensure PWRKEY pin P0.1 is configured as output resting HIGH */
    LPC_GPIO0->FIODIR |= (1UL << PIN_PWR_KEY);
    LPC_GPIO0->FIOSET = (1UL << PIN_PWR_KEY);

    /* 3. Probe if modem is already running */
    clear_rx();
    send_raw("AT\r\n");
    if (!wait_response("OK", 500, NULL, 0)) {
        /* Modem is asleep: pulse PWRKEY LOW for 500ms to boot SIM7600 */
        storage_log_debug("MODEM", "Modem idle, pulsing PWRKEY LOW (500ms) to power on...");
        LPC_GPIO0->FIOCLR = (1UL << PIN_PWR_KEY);
        delay_ms(500);
        LPC_GPIO0->FIOSET = (1UL << PIN_PWR_KEY);
        delay_ms(500);
    }

    /* Allow SIM multiplexer and power supply 2.0s to settle */
    for (int i = 0; i < 20; i++) {
        if (sim7600_is_cancel_requested()) return false;
        delay_ms(100);
    }

    /* 4. Probe modem with "AT" once per second up to 30s or until RDY/OK */
    report_progress("AT PROBING...   ");
    clear_rx();
    bool modem_ready = false;
    for (int i = 0; i < 30; i++) {
        if (sim7600_is_cancel_requested()) return false;
        send_raw("AT\r\n");
        if (wait_response("OK", 800, NULL, 0) || wait_response("RDY", 200, NULL, 0)) {
            modem_ready = true;
            break;
        }
        delay_ms(200);
    }

    if (!modem_ready) {
        storage_log_debug("MODEM", "Modem unresponsive to AT probes");
        report_progress("AT PROBE FAILED ");
        return false;
    }

    report_progress("MODEM READY     ");

    /* 4. Basic modem synchronization */
    send_raw("ATE0\r\n");               /* Echo OFF */
    wait_response("OK", 500, NULL, 0);
    send_raw("AT+CMEE=2\r\n");          /* Verbose error reporting */
    wait_response("OK", 500, NULL, 0);
    send_raw("AT+CGEREP=0\r\n");        /* Silence unsolicited packet domain reports */
    wait_response("OK", 500, NULL, 0);
    send_raw("AT+CREG=0\r\n");          /* Silence registration reports (ESP32 flow) */
    wait_response("OK", 500, NULL, 0);
    send_raw("AT+CEREG=0\r\n");
    wait_response("OK", 500, NULL, 0);
    send_raw("AT&D0\r\n");              /* Ignore DTR line to prevent PPP teardown */
    wait_response("OK", 500, NULL, 0);

    /* 5. Check SIM Card Readiness (AT+CPIN?) */
    report_progress("CHECKING SIM... ");
    char capture[256];
    bool sim_ready = false;
    int not_inserted = 0;
    for (int i = 0; i < 15; i++) {
        if (sim7600_is_cancel_requested()) return false;
        clear_rx();
        send_raw("AT+CPIN?\r\n");
        capture[0] = '\0';
        if (wait_response("+CPIN: READY", 1000, capture, sizeof(capture))) {
            sim_ready = true;
            break;
        }
        /* A definite "not inserted" three times running (the SIM is read in the first second or two):
         * no radio restart or longer wait will find it, so stop now instead of after about 30 s */
        not_inserted = strstr(capture, "not inserted") ? not_inserted + 1 : 0;
        if (not_inserted >= 3) {
            s_sim_missing = true;
            storage_log_debug("ERROR", "No SIM inserted: not trying to register");
            report_progress("NO SIM CARD     ");
            return false;
        }
        delay_ms(1000);
    }

    /* If SIM not ready after 15s, power-cycle radio via CFUN=0 / CFUN=1 once */
    if (!sim_ready) {
        storage_log_debug("MODEM", "SIM not ready, cycling radio CFUN=0 / CFUN=1");
        send_raw("AT+CFUN=0\r\n");
        wait_response("OK", 5000, NULL, 0);
        delay_ms(1000);
        send_raw("AT+CFUN=1\r\n");
        wait_response("OK", 5000, NULL, 0);
        delay_ms(2000);

        /* Still nothing: no SIM can register, so don't spend the battery trying */
        for (int i = 0; i < 5 && !sim_ready; i++) {
            clear_rx();
            send_raw("AT+CPIN?\r\n");
            sim_ready = wait_response("+CPIN: READY", 1000, NULL, 0);
            if (!sim_ready) {
                delay_ms(1000);
            }
        }
        if (!sim_ready) {
            s_sim_missing = true;
            storage_log_debug("ERROR", "No usable SIM: not trying to register");
            report_progress("NO SIM CARD     ");
            return false;
        }
    }
    s_sim_missing = false;

    report_progress("SIM READY       ");

    /* 6. Read ICCID and IMSI for Smart APN Resolution */
    char imsi[20] = "";
    clear_rx();
    send_raw("AT+CICCID\r\n");
    if (wait_response("+ICCID:", 2000, capture, sizeof(capture))) {
        char *p = strstr(capture, "+ICCID: ");
        if (p) {
            sscanf(p, "+ICCID: %20s", s_modem_status.iccid);
        }
    }

    clear_rx();
    send_raw("AT+CIMI\r\n");
    if (wait_response("OK", 2000, capture, sizeof(capture))) {
        char *p = capture;
        while (*p && (*p < '0' || *p > '9')) p++;
        if (*p) {
            sscanf(p, "%15s", imsi);
        }
    }

    const char *apn = select_apn(s_modem_status.iccid, imsi);

    char log_apn[64];
    snprintf(log_apn, sizeof(log_apn), "APN Selected: %s (IMSI: %s, ICCID: %s)", apn, imsi, s_modem_status.iccid);
    storage_log_debug("APN", log_apn);

    /* Configure PDP Context 1 */
    char apn_cmd[64];
    s_active_apn = apn;
    s_bearer_ready = false;
    snprintf(apn_cmd, sizeof(apn_cmd), "AT+CGDCONT=1,\"IP\",\"%s\"\r\n", apn);
    clear_rx();
    send_raw(apn_cmd);
    wait_response("OK", 2000, NULL, 0);

    /* Also configure Socket PDP Context and Active Profile (critical for SIM7600 TCP stack) */
    char sock_cmd[64];
    snprintf(sock_cmd, sizeof(sock_cmd), "AT+CGSOCKCONT=1,\"IP\",\"%s\"\r\n", apn);
    clear_rx();
    send_raw(sock_cmd);
    wait_response("OK", 2000, NULL, 0);

    clear_rx();
    send_raw("AT+CSOCKSETPN=1\r\n");
    wait_response("OK", 1000, NULL, 0);

    /* 7. Radio mode, learned per site (net_policy.c): 4G first, for about twice its usual registration
     * time; where 2G is what works, a short 4G look or none, with a full 4G search every 16th power-on.
     * The state is RATMODE.DAT on SD (fixed length); with no card it starts from the defaults. */
    char rat_saved[NET_POLICY_TEXT_LEN + 8] = "";
    storage_state_load(RAT_MODE_FILENAME, rat_saved, sizeof(rat_saved));
    net_policy_parse(&s_net, rat_saved);
    char old_op[6];
    memcpy(old_op, s_net.op, sizeof(old_op));
    if (net_policy_set_operator(&s_net, imsi)) {
        snprintf(capture, sizeof(capture), "SIM operator changed (%s to %.5s): radio mode learning starts again", old_op, imsi);
        storage_log_debug("MODEM", capture);
    }
    const net_plan_t plan = net_policy_plan(&s_net);
    bool searched_4g = plan.search_4g;
    portTickType mode_switch_tick = 0;
    int rat_mode = plan.search_4g ? 38 : 13;
    char rat_cmd[20];
    snprintf(rat_cmd, sizeof(rat_cmd), "AT+CNMP=%d\r\n", rat_mode);
    send_raw(rat_cmd);
    wait_response("OK", 1000, NULL, 0);
    snprintf(capture, sizeof(capture), plan.search_4g ? "Searching 4G for up to %us%s" : "4G no use here lately: straight to 2G",
             (unsigned)plan.wait_4g_sec, plan.full_probe ? " (full 4G check)" : "");
    storage_log_debug("MODEM", capture);

    /* 8. Registration. Data needs 4G (CEREG) or packet (CGREG) registration: CREG alone is voice and
     * SMS only, and a send then fails after a long wait (1922, 13:15 on 2026-11-08). So with CREG
     * alone it waits a few seconds more for data, then goes on anyway (SMS still works). */
    report_progress("SEARCHING NET...");
    bool registered = false;
    int reg_mode = 0;
    uint8_t sec_to_4g = 0;
    const portTickType search_begin = xTaskGetTickCount();
    portTickType voice_only_since = 0;
    /* The mode is the one the modem was held to when it registered: in 4G-only mode it can only be on
     * 4G, whichever of CEREG/CGREG/CREG reports it first (BIHAR's modem answers CGREG on LTE) */
    #define MODE_NOW() ((rat_mode == 38) ? NET_4G : NET_2G)
    for (int i = 0; i < 25; i++) {
        if (sim7600_is_cancel_requested()) return false;
        uint32_t searched_sec = (uint32_t)((xTaskGetTickCount() - search_begin) / configTICK_RATE_HZ);

        /* 4G skipped as no use here, but 2G isn't answering either (switched off, or another operator's
         * SIM): 4G gets the rest of this search rather than waiting hours for the next full check */
        if (rat_mode == 13 && !searched_4g && searched_sec >= NET_2G_ONLY_WAIT_SEC) {
            rat_mode = 38;
            searched_4g = true;
            mode_switch_tick = xTaskGetTickCount();
            snprintf(rat_cmd, sizeof(rat_cmd), "AT+CNMP=%d\r\n", rat_mode);
            snprintf(capture, sizeof(capture), "No 2G after %lus: trying 4G", (unsigned long)searched_sec);
            storage_log_debug("MODEM", capture);
            send_raw(rat_cmd);
            wait_response("OK", 1000, NULL, 0);
        } else if (rat_mode == 38 && plan.search_4g && searched_sec >= plan.wait_4g_sec) {
            rat_mode = 13;
            snprintf(rat_cmd, sizeof(rat_cmd), "AT+CNMP=%d\r\n", rat_mode);
            snprintf(capture, sizeof(capture), "No 4G after %lus: trying 2G", (unsigned long)searched_sec);
            storage_log_debug("MODEM", capture);
            send_raw(rat_cmd);
            wait_response("OK", 1000, NULL, 0);
        }

        clear_rx();
        send_raw("AT+CEREG?\r\n");
        bool cereg_answered = wait_response("+CEREG: ", 800, capture, sizeof(capture));
        if (cereg_answered && rat_mode == 38 && strstr(capture, "+CEREG: 0,3")) {
            /* The network refused 4G registration (1922, 17:45): waiting out the 4G time can't help */
            rat_mode = 13;
            snprintf(rat_cmd, sizeof(rat_cmd), "AT+CNMP=%d\r\n", rat_mode);
            storage_log_debug("MODEM", "4G registration denied by the network: trying 2G now");
            send_raw(rat_cmd);
            wait_response("OK", 1000, NULL, 0);
            continue;
        }
        if (cereg_answered && (strstr(capture, ",1") || strstr(capture, ",5"))) {
            registered = true;
            reg_mode = NET_4G;
            sec_to_4g = (uint8_t)(((xTaskGetTickCount() - (mode_switch_tick ? mode_switch_tick : search_begin)) / configTICK_RATE_HZ) + 1);
            storage_log_debug("MODEM", "Registered on LTE (+CEREG: 1/5)");
            break;
        }

        clear_rx();
        send_raw("AT+CGREG?\r\n");
        if (wait_response("+CGREG: ", 800, capture, sizeof(capture)) && (strstr(capture, ",1") || strstr(capture, ",5"))) {
            registered = true;
            reg_mode = MODE_NOW();
            storage_log_debug("MODEM", rat_mode == 38 ? "Registered on LTE (+CGREG: 1/5 in 4G-only mode)" : "Registered on GPRS (+CGREG: 1/5)");
            break;
        }

        clear_rx();
        send_raw("AT+CREG?\r\n");
        if (wait_response("+CREG: ", 800, capture, sizeof(capture)) && (strstr(capture, ",1") || strstr(capture, ",5"))) {
            if (voice_only_since == 0) {
                voice_only_since = xTaskGetTickCount();
                storage_log_debug("MODEM", "Registered for voice/SMS only (+CREG): waiting for data registration");
            } else if ((xTaskGetTickCount() - voice_only_since) / configTICK_RATE_HZ >= NET_VOICE_ONLY_WAIT_SEC) {
                registered = true;
                reg_mode = MODE_NOW();
                storage_log_debug("MODEM", "Registered (+CREG: 1/5) without data registration");
                break;
            }
        }

        delay_ms(1000);
    }

    #undef MODE_NOW
    if (reg_mode == NET_4G && sec_to_4g == 0) {
        sec_to_4g = (uint8_t)(((xTaskGetTickCount() - (mode_switch_tick ? mode_switch_tick : search_begin)) / configTICK_RATE_HZ) + 1);
    }
    if (registered) {
        report_progress("NET REGISTERED  ");
    } else {
        report_progress("NO NETWORK      ");
        storage_log_debug("MODEM", "Network registration timeout!");
    }
    s_net_mode = reg_mode;
    net_policy_registered(&s_net, searched_4g, reg_mode, sec_to_4g);
    save_net_policy();

    /* 9. Read Signal Quality (AT+CSQ) */
    report_progress("READING CSQ...  ");
    clear_rx();
    send_raw("AT+CSQ\r\n");
    if (wait_response("+CSQ:", 1000, capture, sizeof(capture))) {
        int csq = 99, ber = 99;
        char *p = strstr(capture, "+CSQ: ");
        if (p) {
            sscanf(p, "+CSQ: %d,%d", &csq, &ber);
            s_modem_status.csq_val = csq;
            s_modem_status.csq_dbm = (csq == 99) ? -113 : (-113 + 2 * csq);
            char csq_msg[48];
            snprintf(csq_msg, sizeof(csq_msg), "CSQ: %d (%d dBm)", csq, s_modem_status.csq_dbm);
            storage_log_debug("MODEM", csq_msg);
        }
    }

    /* 10. Synchronize time from cellular network base station (NITZ) and query carrier */
    if (registered) {
        send_raw("AT+CTZU=1\r\n"); /* Enable auto network time zone & time */
        wait_response("OK", 1000, NULL, 0);

        send_raw("AT+COPS=3,0\r\n");
        wait_response("OK", 1000, NULL, 0);

        clear_rx();
        send_raw("AT+COPS?\r\n");
        if (wait_response("+COPS:", 1500, capture, sizeof(capture))) {
            parse_cops_operator(capture, s_modem_status.carrier, sizeof(s_modem_status.carrier));
        }

        sim7600_sync_time_step();
    }

    s_modem_status.is_registered = registered;
    return registered;
}

typedef enum {
    MODEM_ST_UNINIT = 0,
    MODEM_ST_PROBING,
    MODEM_ST_CONFIGURING,
    MODEM_ST_RUNNING
} modem_worker_state_t;

static modem_worker_state_t s_worker_state = MODEM_ST_UNINIT;
static bool s_time_synced_flag = false;
static uint32_t s_cclk_poll_counter = 0;

void sim7600_power_off(void)
{
    storage_log_debug("MODEM", "Powering down SIM7600 cellular engine...");

    clear_rx();
    close_socket_quiet(500);

    clear_rx();
    send_raw("AT+NETCLOSE\r\n");
    wait_response("OK", 500, NULL, 0);

    /* Software Power Off: AT+CPOF */
    clear_rx();
    send_raw("AT+CPOF\r\n");
    bool cpof_ok = wait_response("OK", 3000, NULL, 0) || wait_response("MS OFF", 1000, NULL, 0);

    /* Hardware fallback: pulse PWRKEY (P0.1) LOW for 2.5s if AT+CPOF failed */
    if (!cpof_ok) {
        storage_log_debug("MODEM", "AT+CPOF unconfirmed, pulsing PWRKEY (P0.1) LOW for 2.5s...");
        LPC_GPIO0->FIOCLR = (1UL << PIN_PWR_KEY);
        delay_ms(2500);
        LPC_GPIO0->FIOSET = (1UL << PIN_PWR_KEY);
        delay_ms(500);
    } else {
        /* Ensure PWRKEY returns to resting HIGH */
        LPC_GPIO0->FIOSET = (1UL << PIN_PWR_KEY);
        delay_ms(500);
    }

    /* Cut 3.8V rail via LPC845 sentry ($f2;) */
    hal_power_modem_off();

    /* Reset worker state so supervisor does not poll while powered down */
    s_worker_state = MODEM_ST_UNINIT;
    s_bearer_ready = false;
    s_modem_status.is_registered = false;
    s_modem_status.is_pdp_active = false;
    s_modem_status.csq_val = 99;
    s_modem_status.csq_dbm = -113;
    strncpy(s_modem_status.carrier, "OFFLINE", sizeof(s_modem_status.carrier) - 1);
    storage_log_debug("MODEM", "SIM7600 power down complete. NETLIGHT off.");
}

void sim7600_worker_step(void)
{
    if (!sim7600_acquire(100)) return;

    char capture[256];

    switch (s_worker_state) {
        case MODEM_ST_UNINIT:
            hal_power_modem_on();
            LPC_GPIO0->FIOCLR = (1UL << PIN_SIM_SEL);
            LPC_GPIO0->FIODIR |= (1UL << PIN_SIM_SEL);
            LPC_GPIO0->FIODIR |= (1UL << PIN_PWR_KEY);
            LPC_GPIO0->FIOSET = (1UL << PIN_PWR_KEY);

            /* Check if already responsive; if not, pulse PWRKEY LOW for 500ms */
            clear_rx();
            send_raw("AT\r\n");
            if (!wait_response("OK", 400, NULL, 0)) {
                LPC_GPIO0->FIOCLR = (1UL << PIN_PWR_KEY);
                delay_ms(500);
                LPC_GPIO0->FIOSET = (1UL << PIN_PWR_KEY);
                delay_ms(500);
            }
            s_worker_state = MODEM_ST_PROBING;
            break;

        case MODEM_ST_PROBING:
            clear_rx();
            send_raw("AT\r\n");
            if (wait_response("OK", 400, NULL, 0) || wait_response("RDY", 200, NULL, 0)) {
                storage_log_debug("MODEM", "SIM7600 responsive to AT probes");
                s_worker_state = MODEM_ST_CONFIGURING;
            }
            break;

        case MODEM_ST_CONFIGURING: {
            send_raw("ATE0\r\n");
            wait_response("OK", 500, NULL, 0);
            send_raw("AT+CMEE=2\r\n");
            wait_response("OK", 500, NULL, 0);
            send_raw("AT+CGEREP=0\r\n");
            wait_response("OK", 500, NULL, 0);
            send_raw("AT&D0\r\n");
            wait_response("OK", 500, NULL, 0);
            send_raw("AT+CTZU=1\r\n");
            wait_response("OK", 500, NULL, 0);
            send_raw("AT+COPS=3,0\r\n");
            wait_response("OK", 500, NULL, 0);
            send_raw("AT+CNMP=2\r\n");
            wait_response("OK", 1000, NULL, 0);

            /* Read ICCID and IMSI for smart APN resolution */
            clear_rx();
            send_raw("AT+CICCID\r\n");
            if (wait_response("+ICCID:", 1500, capture, sizeof(capture))) {
                char *p = strstr(capture, "+ICCID: ");
                if (p) sscanf(p, "+ICCID: %20s", s_modem_status.iccid);
            }

            char imsi[20] = "";
            clear_rx();
            send_raw("AT+CIMI\r\n");
            if (wait_response("OK", 1500, capture, sizeof(capture))) {
                char *p = capture;
                while (*p && (*p < '0' || *p > '9')) p++;
                if (*p) sscanf(p, "%15s", imsi);
            }

            const char *apn = select_apn(s_modem_status.iccid, imsi);

            char apn_cmd[64];
            s_active_apn = apn;
            s_bearer_ready = false;
            snprintf(apn_cmd, sizeof(apn_cmd), "AT+CGDCONT=1,\"IP\",\"%s\"\r\n", apn);
            clear_rx();
            send_raw(apn_cmd);
            wait_response("OK", 1500, NULL, 0);

            char sock_cmd[64];
            snprintf(sock_cmd, sizeof(sock_cmd), "AT+CGSOCKCONT=1,\"IP\",\"%s\"\r\n", apn);
            clear_rx();
            send_raw(sock_cmd);
            wait_response("OK", 1500, NULL, 0);

            send_raw("AT+CSOCKSETPN=1\r\n");
            wait_response("OK", 1000, NULL, 0);

            s_worker_state = MODEM_ST_RUNNING;
            storage_log_debug("MODEM", "SIM7600 background supervisor active");
            break;
        }

        case MODEM_ST_RUNNING: {
            /* 1. Poll CSQ */
            clear_rx();
            send_raw("AT+CSQ\r\n");
            if (wait_response("+CSQ:", 800, capture, sizeof(capture))) {
                int csq = 99, ber = 99;
                char *p = strstr(capture, "+CSQ: ");
                if (p) {
                    sscanf(p, "+CSQ: %d,%d", &csq, &ber);
                    s_modem_status.csq_val = csq;
                    s_modem_status.csq_dbm = (csq == 99) ? -113 : (-113 + 2 * csq);
                }
            }

            /* 2. Poll Registration Status (CEREG / CREG / CGREG) */
            bool reg = false;
            clear_rx();
            send_raw("AT+CEREG?\r\n");
            if (wait_response("+CEREG: ", 800, capture, sizeof(capture))) {
                if (strstr(capture, ",1") || strstr(capture, ",5")) reg = true;
            }
            if (!reg) {
                clear_rx();
                send_raw("AT+CREG?\r\n");
                if (wait_response("+CREG: ", 800, capture, sizeof(capture))) {
                    if (strstr(capture, ",1") || strstr(capture, ",5")) reg = true;
                }
            }
            if (!reg) {
                clear_rx();
                send_raw("AT+CGREG?\r\n");
                if (wait_response("+CGREG: ", 800, capture, sizeof(capture))) {
                    if (strstr(capture, ",1") || strstr(capture, ",5")) reg = true;
                }
            }
            s_modem_status.is_registered = reg;

            /* 3. If registered, query live Carrier and NITZ clock */
            if (reg) {
                /* Poll Carrier Name */
                clear_rx();
                send_raw("AT+COPS?\r\n");
                if (wait_response("+COPS:", 1000, capture, sizeof(capture))) {
                    parse_cops_operator(capture, s_modem_status.carrier, sizeof(s_modem_status.carrier));
                }

                /* Poll Cellular Clock (NITZ) */
                s_cclk_poll_counter++;
                if (!s_time_synced_flag || (s_cclk_poll_counter % 60 == 0)) {
                    if (sim7600_sync_time_step()) {
                        s_time_synced_flag = true;
                    } else if (!s_time_synced_flag && s_cclk_poll_counter >= 3) {
                        /* Fallback to NTP if tower does not broadcast NITZ */
                        sim7600_ntp_sync_step();
                    }
                }

                /* Check incoming SMS */
                telemetry_data_t t;
                if (intermcu_get_telemetry(&t)) {
                    sim7600_check_incoming_sms(&t);
                }
            } else {
                /* If not registered, reset carrier name to searching */
                if (s_modem_status.carrier[0] == '\0') {
                    strncpy(s_modem_status.carrier, "SEARCHING", sizeof(s_modem_status.carrier) - 1);
                }
            }
            break;
        }

        default:
            s_worker_state = MODEM_ST_UNINIT;
            break;
    }
    sim7600_release();
}

static bool apn_field_is(const char *field, const char *apn)
{
    size_t n = strlen(apn);
    for (size_t i = 0; i < n; i++) {
        char x = field[i], y = apn[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (x != y) {
            return false;
        }
    }
    /* The carrier may add ".mncNNN.mccNNN.gprs" */
    return field[n] == '\0' || field[n] == ',' || field[n] == '"' || field[n] == '.' || field[n] == '\r' || field[n] == '\n';
}

/* The carrier's own view of the data connection: the APN and address really in use. Returns false
 * only when it is plainly on another of the APNs this firmware sets (left in the modem from another
 * SIM); an APN name the carrier reports in its own words counts as fine, so it never costs a radio
 * restart. apn NULL: just log it. */
static bool log_bearer_details_apn(const char *apn)
{
    static const char *const known[] = {"airteliot.com", "airtelgprs.com", "bsnlnet", "bsnlm2m", "jionet", NULL};
    char reply[160] = "";
    clear_rx();
    send_raw("AT+CGCONTRDP=1\r\n");
    wait_response("OK", 3000, reply, sizeof(reply));
    if (!apn || !apn[0]) {
        return true;
    }
    /* +CGCONTRDP: <cid>,<bearer id>,<apn>,... */
    const char *field = strstr(reply, "+CGCONTRDP:");
    field = field ? strchr(field, ',') : NULL;
    field = field ? strchr(field + 1, ',') : NULL;
    if (!field) {
        return true;
    }
    field++;
    if (*field == '"') {
        field++;
    }
    if (apn_field_is(field, apn)) {
        return true;
    }
    for (int i = 0; known[i]; i++) {
        if (apn_field_is(field, known[i])) {
            return false;
        }
    }
    return true;
}

static void log_bearer_details(void)
{
    (void)log_bearer_details_apn(NULL);
}

/* ESP32 try_activate_apn, plus AT+CGSOCKCONT and AT+CSOCKSETPN, which the SIM7600's own socket
 * stack needs. CID 1's APN is only rewritten if it differs: this runs up to 96 times a day, and
 * the modem keeps it in flash. */
static bool __attribute__((noinline)) activate_apn(const char *apn)
{
    const char *pdp_type = (strcmp(apn, "jionet") == 0) ? "IPV4V6" : "IP";
    char cmd[64];
    char reply[256] = "";
    char log_msg[72];

    snprintf(log_msg, sizeof(log_msg), "Activating data connection on APN %s", apn);
    storage_log_debug("APN", log_msg);
    report_progress("ACTIVATING DATA ");

    /* The ESP32 blanks these side channels. CID 5 came from older firmware here: delete it. */
    clear_rx();
    send_raw("AT+CGDCONT=5\r\n");
    wait_response("OK", 1000, NULL, 0);
    clear_rx();
    send_raw("AT+CGDCONT=8,\"IP\",\"\"\r\n");
    wait_response("OK", 1000, NULL, 0);
    clear_rx();
    send_raw("AT+CGDCONT=9,\"IP\",\"\"\r\n");
    wait_response("OK", 1000, NULL, 0);

    snprintf(cmd, sizeof(cmd), "+CGDCONT: 1,\"%s\",\"%s\"", pdp_type, apn);
    clear_rx();
    send_raw("AT+CGDCONT?\r\n");
    bool apn_stored = wait_response("OK", 2000, reply, sizeof(reply)) && strstr(reply, cmd);
    if (!apn_stored) {
        snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"%s\",\"%s\"\r\n", pdp_type, apn);
        clear_rx();
        send_raw(cmd);
        wait_response("OK", 3000, NULL, 0);
    }
    delay_ms(800);

    snprintf(cmd, sizeof(cmd), "AT+CGSOCKCONT=1,\"%s\",\"%s\"\r\n", pdp_type, apn);
    clear_rx();
    send_raw(cmd);
    wait_response("OK", 2000, NULL, 0);
    clear_rx();
    send_raw("AT+CSOCKSETPN=1\r\n");
    wait_response("OK", 1000, NULL, 0);

    /* Two tries, with an attach between. BSNL's "no network service" on 1922 (2026-11-09) cleared on
     * the second try once (10:45); 3 times only the radio restart that follows cleared it, and 5
     * tries over 13 s (v6.9.24, 14:00) didn't help, so more tries only add time. */
    bool active = false;
    for (int attempt = 0; attempt < 2 && !active; attempt++) {
        clear_rx();
        send_raw("AT+CGACT=1,1\r\n");
        active = wait_response("OK", 25000, NULL, 0);
        if (!active && attempt == 0) {
            clear_rx();
            send_raw("AT+CGATT=1\r\n");
            wait_response("OK", 3000, NULL, 0);
            delay_ms(1500);
        }
    }

    /* A SIM7600 can answer ERROR when CID 1 is already up, so ask rather than assume */
    if (!active) {
        reply[0] = '\0';
        clear_rx();
        send_raw("AT+CGACT?\r\n");
        active = wait_response("OK", 3000, reply, sizeof(reply)) && strstr(reply, "+CGACT: 1,1");
    }
    if (!active) {
        storage_log_debug("APN", "Data connection did not activate");
        return false;
    }

    /* After a cold start the address can take a few seconds to arrive */
    for (int i = 0; i < 8; i++) {
        delay_ms(i == 0 ? 1000 : 2500);
        reply[0] = '\0';
        clear_rx();
        send_raw("AT+CGPADDR=1\r\n");
        if (wait_response("OK", 3000, reply, sizeof(reply)) && !strstr(reply, "0.0.0.0")) {
            return true;
        }
    }
    storage_log_debug("APN", "Activated, but no address (0.0.0.0) after 20s: treating as failed");
    return false;
}

/* An attached LTE modem keeps the APN it attached with, so the radio restarts for the change.
 * AT+CFUN=4 rather than 0: at 0 the SIM drops out and AT+CGDCONT answers ERROR. */
static bool __attribute__((noinline)) reattach_on_apn(const char *apn)
{
    char cmd[64];
    char reg[64];

    clear_rx();
    send_raw("AT+NETCLOSE\r\n");
    wait_response("+NETCLOSE", 3000, NULL, 0);

    clear_rx();
    send_raw("AT+CFUN=4\r\n");
    wait_response("OK", 5000, NULL, 0);
    delay_ms(1000);

    snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"%s\",\"%s\"\r\n", (strcmp(apn, "jionet") == 0) ? "IPV4V6" : "IP", apn);
    clear_rx();
    send_raw(cmd);
    wait_response("OK", 3000, NULL, 0);

    clear_rx();
    send_raw("AT+CFUN=1\r\n");
    wait_response("OK", 5000, NULL, 0);

    s_active_apn = apn;
    s_bearer_ready = false;

    for (int i = 0; i < 15; i++) {
        delay_ms(2000);
        for (int which = 0; which < 2; which++) {
            reg[0] = '\0';
            clear_rx();
            send_raw(which ? "AT+CGREG?\r\n" : "AT+CEREG?\r\n");
            if (wait_response(which ? "+CGREG:" : "+CEREG:", 1500, reg, sizeof(reg)) && (strstr(reg, ",1") || strstr(reg, ",5"))) {
                return true;
            }
        }
    }
    storage_log_debug("APN", "Did not register on this APN");
    return false;
}

/* The SIM's APN; then a fresh attach on that same APN (what cleared "no network service" on 1922 after
 * BSNL 4G denied it, 2026-11-08); then only that operator's other APNs. Another operator's APN can't
 * work on a known SIM, and each try restarts the radio, so the full list is kept for a SIM whose
 * operator isn't known. Then a CGATT reset. The whole search stops after about 3 minutes. */
#define BEARER_SEARCH_MAX_TICKS (180000 / portTICK_RATE_MS)

static void __attribute__((noinline)) ensure_bearer(void)
{
    static const char *const all_apns[] = {"airteliot.com", "airtelgprs.com", "bsnlnet", "jionet", "bsnlm2m", NULL};
    static const char *const bsnl_apns[] = {"bsnlnet", "bsnlm2m", NULL};
    static const char *const airtel_apns[] = {"airteliot.com", "airtelgprs.com", NULL};
    static const char *const one_apn[] = {NULL};     /* Jio, Vi: their own APN only */
    const char *const *fallback_apns = (s_apn_op == APN_OP_UNKNOWN) ? all_apns :
                                       (s_apn_op == APN_OP_BSNL) ? bsnl_apns :
                                       (s_apn_op == APN_OP_AIRTEL) ? airtel_apns : one_apn;

    if (s_bearer_ready) {
        return;
    }

    const char *primary = s_active_apn;
    portTickType start = xTaskGetTickCount();

    s_bearer_ready = primary[0] && activate_apn(primary);
    bool details_logged = s_bearer_ready; /* The APN check below logs them */
    if (s_bearer_ready && !log_bearer_details_apn(primary)) {
        details_logged = false;
        /* The modem attaches by itself at power-on with the APN it last stored, before this sets the
         * new one: after a SIM swap the data connection can be up on the old operator's APN. BSNL
         * then gave an address on airteliot.com but NETOPEN failed twice (BIHAR 23:45, 2026-11-08,
         * 72 s). Attach again on the right APN before sending. */
        storage_log_debug("APN", "Data connection is on another APN: fresh attach on the SIM's APN");
        s_bearer_ready = reattach_on_apn(primary) && activate_apn(primary);
    }
    if (!s_bearer_ready && primary[0]) {
        storage_log_debug("APN", "Fresh attach on the same APN");
        s_bearer_ready = reattach_on_apn(primary) && activate_apn(primary);
    }

    for (size_t i = 0; fallback_apns[i] && !s_bearer_ready; i++) {
        if (strcmp(fallback_apns[i], primary) == 0 || (xTaskGetTickCount() - start) > BEARER_SEARCH_MAX_TICKS) {
            continue;
        }
        s_bearer_ready = reattach_on_apn(fallback_apns[i]) && activate_apn(fallback_apns[i]);
    }

    if (!s_bearer_ready && primary[0]) {
        storage_log_debug("APN", "All APNs failed: resetting with AT+CGATT");
        reattach_on_apn(primary);
        clear_rx();
        send_raw("AT+CGATT=0\r\n");
        wait_response("OK", 5000, NULL, 0);
        delay_ms(1000);
        clear_rx();
        send_raw("AT+CGATT=1\r\n");
        wait_response("OK", 5000, NULL, 0);
        delay_ms(2000);
        s_bearer_ready = activate_apn(primary);
    }

    if (!details_logged || !s_bearer_ready) {
        log_bearer_details();
    }
}

/* "+IPD" only says a reply has started. Collect the rest, up to 8s as the ESP32 does, because a
 * "Rejected" in the body can arrive after the "200 OK" in the header. */
#define HTTP_REPLY_MAX_MS   8000
#define HTTP_REPLY_QUIET_MS 1500

static void collect_http_reply(char *buf, size_t size)
{
    size_t start = strlen(buf);
    size_t len = start;
    uint32_t quiet_ms = 0;

    for (uint32_t elapsed = 0; elapsed < HTTP_REPLY_MAX_MS; elapsed += 20) {
        bool got_data = false;
        while (s_rx_tail != s_rx_head) {
            char c = s_rx_buf[s_rx_tail];
            s_rx_tail = (s_rx_tail + 1) % RX_BUFFER_SIZE;
            got_data = true;
            if (len < size - 1) {
                buf[len++] = c;
            }
        }
        buf[len] = '\0';

        if (strstr(buf, "+IPCLOSE")) {
            break; /* Server has finished and closed the connection */
        }
        quiet_ms = got_data ? 0 : quiet_ms + 20;
        if (quiet_ms >= HTTP_REPLY_QUIET_MS && strstr(buf, "HTTP/1.")) {
            break;
        }
        delay_ms(20);
    }

    if (len > start) {
        storage_log_debug("MODEM_RX", &buf[start]);
    }
}

/* Direct TCP socket. *got_reply says the server answered, so its verdict stands. */
/* The request went out and no answer came back, so the server may well have it. It's not counted
 * as delivered, but it waits for the next slot rather than going straight out again. */
static bool s_post_unanswered = false;
static bool s_post_rejected = false;

bool sim7600_post_rejected(void)
{
    return s_post_rejected;
}

/* Checks the network is still there before spending more battery on it */
bool sim7600_still_registered(void)
{
    static const char *const cmds[] = { "AT+CEREG?\r\n", "AT+CGREG?\r\n", "AT+CREG?\r\n" };
    static const char *const keys[] = { "+CEREG: ", "+CGREG: ", "+CREG: " };
    char capture[64];
    for (int i = 0; i < 3; i++) {
        clear_rx();
        send_raw(cmds[i]);
        if (wait_response(keys[i], 800, capture, sizeof(capture)) &&
            (strstr(capture, ",1") || strstr(capture, ",5"))) {
            return true;
        }
    }
    return false;
}

bool sim7600_post_unanswered(void)
{
    return s_post_unanswered;
}

static bool __attribute__((noinline)) http_post_tcp(const http_endpoint_t *endpoint, const char *host, const char *payload, char *date_hdr_out, size_t date_hdr_size, bool *got_reply)
{
    char log_msg[128];
    char capture[256] = "";

    /* Step 1: Ensure IP Stack (NETOPEN) is active */
    bool net_is_open = false;
    clear_rx();
    send_raw("AT+NETOPEN?\r\n");
    if (wait_response("+NETOPEN:", 2000, capture, sizeof(capture))) {
        if (strstr(capture, "+NETOPEN: 1")) {
            net_is_open = true;
            storage_log_debug("TCP", "NETOPEN already open (+NETOPEN: 1)");
        }
    }

    if (!net_is_open) {
        /* Only accepted while the network is closed */
        clear_rx();
        send_raw("AT+CIPMODE=0\r\n");
        wait_response("OK", 1000, NULL, 0);

        clear_rx();
        send_raw("AT+NETOPEN\r\n");
        /* SIM7600 responds OK immediately, then asynchronously +NETOPEN: 0 on bearer activation */
        if (wait_response("+NETOPEN: 0", 20000, capture, sizeof(capture)) ||
            strstr(capture, "already opened")) {
            storage_log_debug("TCP", "NETOPEN successful (+NETOPEN: 0)");
            net_is_open = true;
        } else {
            snprintf(log_msg, sizeof(log_msg), "NETOPEN failed! (Response: %.60s)", capture);
            storage_log_debug("TCP", log_msg);
            return false;
        }
    }

    /* Step 2: Query IP address for verification */
    clear_rx();
    send_raw("AT+IPADDR\r\n");
    if (wait_response("+IPADDR:", 3000, capture, sizeof(capture))) {
        char *p = strstr(capture, "+IPADDR: ");
        if (p) {
            char ip_str[32] = "";
            sscanf(p, "+IPADDR: %31s", ip_str);
            snprintf(log_msg, sizeof(log_msg), "Assigned IP: %s", ip_str);
            storage_log_debug("TCP", log_msg);
            /* No usable IP means a dead data connection: restart it on the next attempt */
            if (ip_str[0] == '\0' || strstr(ip_str, "0.0.0.0")) {
                storage_log_debug("TCP", "No valid IP on data connection, closing network");
                send_raw("AT+NETCLOSE\r\n");
                wait_response("OK", 2000, NULL, 0);
                return false;
            }
            strncpy(s_modem_status.ip_addr, ip_str, sizeof(s_modem_status.ip_addr) - 1);
        }
    }

    /* Step 3: Close any stale socket 0 connection */
    close_socket_quiet(1000);
    delay_ms(100);

    /* Step 4: Open Layer-4 direct TCP socket to endpoint */
    char open_cmd[128];
    snprintf(open_cmd, sizeof(open_cmd), "AT+CIPOPEN=0,\"TCP\",\"%s\",%d\r\n", host, endpoint->port);
    snprintf(log_msg, sizeof(log_msg), "Connecting to %s:%d...", host, endpoint->port);
    storage_log_debug("TCP", log_msg);

    clear_rx();
    send_raw(open_cmd);
    if (!wait_response("+CIPOPEN: 0,0", 20000, capture, sizeof(capture))) {
        snprintf(log_msg, sizeof(log_msg), "CIPOPEN failed! (Response: %.60s)", capture);
        storage_log_debug("TCP", log_msg);
        if (!strstr(capture, "+CIPOPEN:")) {
            /* No answer at all: socket 0 is still opening and AT+CIPCLOSE doesn't stop that, so the
             * next try got "+CIPOPEN: 0,4 ERROR" at once (BIHAR 07:00, 2026-11-09). Closing the
             * network clears it; the next try opens it again. */
            clear_rx();
            send_raw("AT+NETCLOSE\r\n");
            wait_response("+NETCLOSE", 3000, NULL, 0);
        }
        return false;
    }
    storage_log_debug("TCP", "TCP socket connected (+CIPOPEN: 0,0 OK)!");

    delay_ms(150);

    /* Step 5: Format HTTP Request Headers */
    const char *content_type = (endpoint->format == FORMAT_JSON) ? "application/json" : "application/x-www-form-urlencoded";
    /* Host is what we connected to, as in the TWS6 firmware that delivers on this board */
    char port_text[8] = "";
    if (endpoint->port != 80) {
        snprintf(port_text, sizeof(port_text), ":%d", endpoint->port);
    }
    int req_len = snprintf(s_http_req, sizeof(s_http_req),
        "POST %s HTTP/1.1\r\n"
        "Host: %s%s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        endpoint->path, host, port_text, content_type, (int)strlen(payload), payload
    );

    /* Step 6: Send Data */
    char send_cmd[32];
    snprintf(send_cmd, sizeof(send_cmd), "AT+CIPSEND=0,%d\r\n", req_len);
    clear_rx();
    send_raw(send_cmd);
    if (!wait_response(">", 5000, NULL, 0)) {
        storage_log_debug("TCP", "CIPSEND failed: No '>' prompt!");
        close_socket_quiet(0);
        return false;
    }

    storage_log_debug("TCP", "Streaming HTTP payload...");
    clear_rx();
    s_post_unanswered = true;
    send_raw(s_http_req);

    /* Step 7: Wait for Server Response */
    bool success = false;
    s_http_capture[0] = '\0';
    /* "+IP" catches +IPD and +IPCLOSE: a closed socket can't answer, so stop waiting */
    if (wait_response("+IP", 15000, s_http_capture, sizeof(s_http_capture)) && !strstr(s_http_capture, "+IPD")) {
        storage_log_debug("HTTP", "Connection closed with no reply from the server");
    } else if (strstr(s_http_capture, "+IPD")) {
        storage_log_debug("TCP", "Received server response (+IPD)");
        *got_reply = true;
        s_post_unanswered = false;
        collect_http_reply(s_http_capture, sizeof(s_http_capture));
        storage_log_http(s_http_capture);

        /* Parse and log HTTP status line or first line of response */
        char resp_line[128] = "";
        char *http_ver = strstr(s_http_capture, "HTTP/1.");
        if (http_ver) {
            char *eol = strstr(http_ver, "\r\n");
            size_t slen = eol ? (size_t)(eol - http_ver) : 32;
            if (slen >= sizeof(resp_line)) slen = sizeof(resp_line) - 1;
            strncpy(resp_line, http_ver, slen);
            resp_line[slen] = '\0';
        } else {
            snprintf(resp_line, sizeof(resp_line), "%.60s", s_http_capture);
        }
        char log_resp[160];
        snprintf(log_resp, sizeof(log_resp), "Server Status: %s", resp_line);
        storage_log_debug("HTTP", log_resp);

        /* These servers can answer HTTP 200 with "Rejected" in the body: that is not delivered */
        if (strstr(s_http_capture, "Rejected") ||
            strstr(s_http_capture, "rejected") ||
            strstr(s_http_capture, "REJECTED") ||
            strstr(s_http_capture, "Invalid") ||
            strstr(s_http_capture, "invalid")) {
            s_post_rejected = true;
            storage_log_debug("HTTP", "Server REJECTED the record");
            sync_clock_from_server(s_http_capture, true);
        } else if (strstr(s_http_capture, "200 OK") ||
            strstr(s_http_capture, "Success") ||
            strstr(s_http_capture, "SUCCESS") ||
            strstr(s_http_capture, "accepted") ||
            strstr(s_http_capture, "ACCEPTED")) {
            success = true;
            storage_log_debug("HTTP", "HTTP POST Verified OK (200 / Success)");
            sync_clock_from_server(s_http_capture, false);

            /* Extract server Date: header if present to update RTC */
            char *d_ptr = strstr(s_http_capture, "Date: ");
            if (d_ptr && date_hdr_out && date_hdr_size > 0) {
                char *end = strstr(d_ptr, "\r\n");
                if (end) {
                    size_t len = end - d_ptr;
                    if (len >= date_hdr_size) len = date_hdr_size - 1;
                    strncpy(date_hdr_out, d_ptr, len);
                    date_hdr_out[len] = '\0';
                }
            }
        } else {
            storage_log_debug("HTTP", "Server response rejected (no 200 OK / Success matched)");
        }
    } else {
        storage_log_debug("HTTP", "HTTP Response timeout (no +IPD received within 15s)!");
    }

    /* Step 8: Close TCP socket */
    close_socket_quiet(1000);

    return success;
}

/* The modem's own HTTP commands: the ESP32's fallback when a TCP post gets no answer */
/* The modem's HTTP commands give only the body unless asked for the headers, where Date is */
static void sync_clock_after_modem_http(bool rejected)
{
    char ts[16];
    if (!intermcu_parse_server_time(s_http_capture, ts, sizeof(ts))) {
        s_http_capture[0] = '\0';
        clear_rx();
        send_raw("AT+HTTPHEAD\r\n");
        wait_response("+HTTPHEAD:", 3000, s_http_capture, sizeof(s_http_capture));
    }
    sync_clock_from_server(s_http_capture, rejected);
}

static bool __attribute__((noinline)) http_post_modem_stack(const http_endpoint_t *endpoint, const char *payload)
{
    const char *content_type = (endpoint->format == FORMAT_JSON) ? "application/json" : "application/x-www-form-urlencoded";
    char log_msg[96];
    bool success = false;

    storage_log_debug("HTTP", "TCP post unconfirmed, trying modem HTTP commands");

    /* A session left open makes HTTPINIT fail */
    clear_rx();
    send_raw("AT+HTTPTERM\r\n");
    wait_response("OK", 1000, NULL, 0);

    clear_rx();
    send_raw("AT+HTTPINIT\r\n");
    if (!wait_response("OK", 5000, NULL, 0)) {
        storage_log_debug("HTTP", "AT+HTTPINIT failed");
        return false;
    }

    if (endpoint->port == 80) {
        snprintf(s_http_req, sizeof(s_http_req), "AT+HTTPPARA=\"URL\",\"http://%s%s\"\r\n", endpoint->domain, endpoint->path);
    } else {
        snprintf(s_http_req, sizeof(s_http_req), "AT+HTTPPARA=\"URL\",\"http://%s:%d%s\"\r\n", endpoint->domain, endpoint->port, endpoint->path);
    }
    clear_rx();
    send_raw(s_http_req);
    wait_response("OK", 1000, NULL, 0);

    snprintf(s_http_req, sizeof(s_http_req), "AT+HTTPPARA=\"CONTENT\",\"%s\"\r\n", content_type);
    clear_rx();
    send_raw(s_http_req);
    wait_response("OK", 1000, NULL, 0);

    clear_rx();
    send_raw("AT+HTTPPARA=\"ACCEPT\",\"*/*\"\r\n");
    wait_response("OK", 1000, NULL, 0);

    snprintf(s_http_req, sizeof(s_http_req), "AT+HTTPDATA=%d,5000\r\n", (int)strlen(payload));
    clear_rx();
    send_raw(s_http_req);
    if (!wait_response("DOWNLOAD", 5000, NULL, 0)) {
        storage_log_debug("HTTP", "AT+HTTPDATA: no DOWNLOAD prompt");
        goto done;
    }

    clear_rx();
    send_raw(payload);
    if (!wait_response("OK", 6000, NULL, 0)) {
        storage_log_debug("HTTP", "AT+HTTPDATA: payload not confirmed");
        goto done;
    }

    clear_rx();
    s_post_unanswered = true;
    send_raw("AT+HTTPACTION=1\r\n");
    if (!wait_response("+HTTPACTION:", 25000, s_http_capture, sizeof(s_http_capture))) {
        storage_log_debug("HTTP", "AT+HTTPACTION: no result in 25s");
        goto done;
    }

    /* +HTTPACTION: 1,<status>,<length> */
    int status = 0;
    int length = 0;
    const char *field = strstr(s_http_capture, "+HTTPACTION:");
    field = field ? strchr(field, ',') : NULL;
    if (field) {
        status = atoi(field + 1);
        field = strchr(field + 1, ',');
        length = field ? atoi(field + 1) : 0;
    }
    snprintf(log_msg, sizeof(log_msg), "Modem HTTP status %d, body %d bytes", status, length);
    storage_log_debug("HTTP", log_msg);
    /* 6xx and 7xx are the modem's own errors, not the server's answer */
    if (status >= 100 && status <= 599) {
        s_post_unanswered = false;
    }
    if (status < 200 || status > 202) {
        goto done;
    }

    s_http_capture[0] = '\0';
    if (length > 0) {
        clear_rx();
        send_raw("AT+HTTPREAD=0,512\r\n");
        wait_response("+HTTPREAD:", 10000, s_http_capture, sizeof(s_http_capture));
    }
    storage_log_http(s_http_capture);

    if (strstr(s_http_capture, "Rejected") || strstr(s_http_capture, "rejected") ||
        strstr(s_http_capture, "REJECTED") || strstr(s_http_capture, "Invalid") ||
        strstr(s_http_capture, "invalid")) {
        s_post_rejected = true;
            storage_log_debug("HTTP", "Server REJECTED the record");
        sync_clock_after_modem_http(true);
    } else {
        success = true;
        storage_log_debug("HTTP", "HTTP POST Verified OK through modem HTTP commands");
        sync_clock_after_modem_http(false);
    }

done:
    clear_rx();
    send_raw("AT+HTTPTERM\r\n");
    wait_response("OK", 2000, NULL, 0);
    return success;
}

/* The name comes first, so a server can move to a new IP with no firmware change. The IP is there
 * for when the name can't be looked up. */
const char *sim7600_route_name(int route)
{
    switch (route) {
    case 0:  return "NAME";
    case 1:  return "IP";
    default: return "MODEM_HTTP";
    }
}

static bool post_by_route(const http_endpoint_t *endpoint, const char *payload, int route,
                          char *date_hdr_out, size_t date_hdr_size, bool *got_reply)
{
    char msg[64];
    bool ok;
    snprintf(msg, sizeof(msg), "Sending by %s (%s)", sim7600_route_name(route),
             (route == 1) ? endpoint->ip : endpoint->domain);
    storage_log_debug("HTTP", msg);

    if (route == 0) {
        ok = http_post_tcp(endpoint, endpoint->domain, payload, date_hdr_out, date_hdr_size, got_reply);
    } else if (route == 1) {
        ok = http_post_tcp(endpoint, endpoint->ip, payload, date_hdr_out, date_hdr_size, got_reply);
    } else {
        ok = http_post_modem_stack(endpoint, payload);
    }
    snprintf(msg, sizeof(msg), "%s by %s", ok ? "Delivered" : "Not delivered", sim7600_route_name(route));
    storage_log_debug("HTTP", msg);
    return ok;
}

/* One send by one route: the caller decides how many it gets */
bool sim7600_http_post_once(const http_endpoint_t *endpoint, const char *payload, int route)
{
    if (!endpoint || !payload) return false;

    ensure_bearer();

    bool got_reply = false;
    s_post_unanswered = false;
    s_post_rejected = false;
    return post_by_route(endpoint, payload, route, NULL, 0, &got_reply);
}

bool sim7600_http_post(const http_endpoint_t *endpoint, const char *payload, char *date_hdr_out, size_t date_hdr_size)
{
    if (!endpoint || !payload) return false;

    ensure_bearer();

    bool got_reply = false;
    s_post_unanswered = false;
    s_post_rejected = false;
    /* If the server answered "no", asking again another way won't change that, and if it may have
     * the record, the next slot sends it. */
    bool success = false;
    for (int route = 0; route < 3 && !success && !got_reply && !s_post_unanswered; route++) {
        if (route > 0) {
            delay_ms(1000);
        }
        success = post_by_route(endpoint, payload, route, date_hdr_out, date_hdr_size, &got_reply);
    }

    if (!success && !got_reply) {
        log_bearer_details();
    }
    return success;
}

#if ENABLE_TEST_SEND
/* TEMPORARY: runs after a failed TEST SEND to show which layer drops the data. The GETs store
 * nothing. Every result is in the log under [DIAG]. */
static void diag_query(const char *cmd)
{
    clear_rx();
    send_raw(cmd);
    wait_response("OK", 2000, NULL, 0);
}

/* A socket can linger after its reply: restart the network so each check starts clean */
static void diag_restart_network(void)
{
    clear_rx();
    send_raw("AT+NETCLOSE\r\n");
    wait_response("+NETCLOSE", 5000, NULL, 0);
    delay_ms(500);
    clear_rx();
    send_raw("AT+NETOPEN\r\n");
    wait_response("+NETOPEN: 0", 10000, NULL, 0);
}

static void __attribute__((noinline)) diag_get(const char *host, int port, const char *path)
{
    char line[112];
    int len = snprintf(s_http_req, sizeof(s_http_req), "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
    const char *result = "COULD NOT CONNECT";

    clear_rx();
    close_socket_quiet(1000);

    snprintf(line, sizeof(line), "AT+CIPOPEN=0,\"TCP\",\"%s\",%d\r\n", host, port);
    clear_rx();
    send_raw(line);
    if (wait_response("+CIPOPEN: 0,0", 20000, NULL, 0)) {
        snprintf(line, sizeof(line), "AT+CIPSEND=0,%d\r\n", len);
        clear_rx();
        send_raw(line);
        result = "NO SEND PROMPT";
        if (wait_response(">", 5000, NULL, 0)) {
            s_http_capture[0] = '\0';
            clear_rx();
            send_raw(s_http_req);
            result = "NO RESPONSE IN 15S";
            if (wait_response("+IP", 15000, s_http_capture, sizeof(s_http_capture))) {
                if (strstr(s_http_capture, "+IPD")) {
                    collect_http_reply(s_http_capture, sizeof(s_http_capture));
                    result = "GOT A REPLY";
                } else {
                    result = "CLOSED WITH NO REPLY";
                }
            }
        }
    }
    clear_rx();
    close_socket_quiet(1000);

    snprintf(line, sizeof(line), "GET %s:%d%s -> %s", host, port, path, result);
    storage_log_debug("DIAG", line);
    diag_restart_network();
}

/* The POST with AT+CIPRXGET=1 (manual receive), the mode of the gprs_v2 flow known to work with
 * this SIM. The mode can only change while the network is closed. */
static bool __attribute__((noinline)) diag_post_rxget(const http_endpoint_t *endpoint, const char *payload)
{
    char line[80];
    bool sent = false;
    const char *result = "COULD NOT CONNECT";
    int len = snprintf(s_http_req, sizeof(s_http_req),
        "POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: %s\r\n"
        "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
        endpoint->path, endpoint->ip,
        (endpoint->format == FORMAT_JSON) ? "application/json" : "application/x-www-form-urlencoded",
        (int)strlen(payload), payload);

    clear_rx();
    send_raw("AT+NETCLOSE\r\n");
    wait_response("+NETCLOSE", 3000, NULL, 0);
    diag_query("AT+CIPRXGET=1\r\n");
    clear_rx();
    send_raw("AT+NETOPEN\r\n");
    wait_response("+NETOPEN: 0", 10000, NULL, 0);

    snprintf(line, sizeof(line), "AT+CIPOPEN=0,\"TCP\",\"%s\",%d\r\n", endpoint->ip, endpoint->port);
    clear_rx();
    send_raw(line);
    if (wait_response("+CIPOPEN: 0,0", 20000, NULL, 0)) {
        snprintf(line, sizeof(line), "AT+CIPSEND=0,%d\r\n", len);
        clear_rx();
        send_raw(line);
        result = "NO SEND PROMPT";
        if (wait_response(">", 5000, NULL, 0)) {
            s_http_capture[0] = '\0';
            clear_rx();
            send_raw(s_http_req);
            result = "NO RESPONSE IN 15S";
            /* Wait past our own send confirmation for what happens next */
            wait_response("+CIPSEND:", 5000, s_http_capture, sizeof(s_http_capture));
            if (!strstr(s_http_capture, "+CIPRXGET: 1") && !strstr(s_http_capture, "+IPCLOSE")) {
                wait_response("+", 15000, s_http_capture, sizeof(s_http_capture));
            }
            if (strstr(s_http_capture, "+CIPRXGET: 1")) {
                s_http_capture[0] = '\0';
                clear_rx();
                send_raw("AT+CIPRXGET=2,0,512\r\n");
                wait_response("OK", 5000, s_http_capture, sizeof(s_http_capture));
                storage_log_http(s_http_capture);
                result = "GOT A REPLY";
                sent = strstr(s_http_capture, "200 OK") && !strstr(s_http_capture, "Rejected") && !strstr(s_http_capture, "Invalid");
            } else if (strstr(s_http_capture, "+IPCLOSE")) {
                result = "CLOSED WITH NO REPLY";
            }
        }
    }
    clear_rx();
    close_socket_quiet(1000);
    clear_rx();
    send_raw("AT+NETCLOSE\r\n");
    wait_response("+NETCLOSE", 3000, NULL, 0);
    diag_query("AT+CIPRXGET=0\r\n");

    snprintf(line, sizeof(line), "POST with AT+CIPRXGET=1 -> %s%s", result, sent ? ", DELIVERED" : "");
    storage_log_debug("DIAG", line);
    return sent;
}

bool sim7600_net_diagnostics(const http_endpoint_t *endpoint, const char *payload)
{
    storage_log_debug("DIAG", "=== Checks after a failed send ===");
    report_progress("RUNNING CHECKS  ");

    /* 1. Modem firmware and its stored socket settings */
    diag_query("AT+CGMR\r\n");
    diag_query("AT+CIPCCFG?\r\n");
    diag_query("AT+CIPTIMEOUT?\r\n");
    diag_query("AT+CIPSENDMODE?\r\n");
    diag_query("AT+CIPHEAD?\r\n");
    diag_query("AT+CIPRXGET?\r\n");

    /* 2. Is the data connection up, and can it reach the server at all? */
    clear_rx();
    send_raw("AT+NETOPEN\r\n");
    wait_response("+NETOPEN: 0", 10000, NULL, 0);

    char ping[64];
    snprintf(ping, sizeof(ping), "AT+CPING=\"%s\",1,3,64,1000,10000,255\r\n", endpoint->ip);
    clear_rx();
    send_raw(ping);
    wait_response("+CPING: 3", 20000, NULL, 0);

    /* 3. A reply from this server to a GET says the POST is what gets dropped. A reply from other
     *    sites, but not this one, points at the route to this server. The gprs_v2 flow known to
     *    work with this SIM used the test server on 8090 and Bihar on 8085, never port 80, so
     *    those two separate this firmware from the port. */
    diag_restart_network();
    diag_get(endpoint->ip, endpoint->port, endpoint->path);
    diag_get("rtdas.ksndmc.net", 80, "/tws_gprs/update_tws_data_v3");
    diag_get("example.com", 80, "/");
    diag_get("sittest5.eastus.cloudapp.azure.com", 8090, "/trg/test");
    diag_get("115.245.49.114", 8085, "/Home/UpdateTRGData"); /* The ESP32's active Bihar server */

    /* 4. The receive mode of the flow known to work with this SIM */
    if (diag_post_rxget(endpoint, payload)) {
        return true;
    }
    clear_rx();
    send_raw("AT+NETOPEN\r\n");
    wait_response("+NETOPEN: 0", 10000, NULL, 0);

    /* 5. TWS6 never sends AT&D0, and this close code can mean DTR went off */
    bool got_reply = false;
    storage_log_debug("DIAG", "Repeating the POST with AT&D1 (TWS6 does not set AT&D0)");
    diag_query("AT&D1\r\n");
    bool sent = http_post_tcp(endpoint, endpoint->ip, payload, NULL, 0, &got_reply);
    diag_query("AT&D0\r\n");

    storage_log_debug("DIAG", sent ? "POST with AT&D1: DELIVERED" :
                               got_reply ? "POST with AT&D1: server replied, record refused" :
                               "POST with AT&D1: no reply");
    return sent;
}
#endif /* ENABLE_TEST_SEND */

/* GPS_PARSE_BEGIN */
/* One ddmm.mmmmmm or dddmm.mmmmmm coordinate to millionths of a degree, with no floats */
static bool gps_parse_coord(const char *p, char dir_pos, char dir_neg, int32_t *out)
{
    uint32_t whole = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9' && digits < 6) {
        whole = whole * 10 + (uint32_t)(*p++ - '0');
        digits++;
    }
    if (digits < 3 || *p != '.') {
        return false;
    }
    p++;

    uint32_t minute_micro = 0;
    for (int i = 0; i < 6; i++) {
        minute_micro *= 10;
        if (*p >= '0' && *p <= '9') {
            minute_micro += (uint32_t)(*p++ - '0');
        }
    }
    while (*p >= '0' && *p <= '9') {
        p++;
    }
    if (*p != ',') {
        return false;
    }
    p++;
    if (*p != dir_pos && *p != dir_neg) {
        return false;
    }

    uint32_t minutes = whole % 100;
    if (minutes >= 60) {
        return false;
    }
    int32_t value = (int32_t)((whole / 100) * 1000000UL + (minutes * 1000000UL + minute_micro) / 60UL);
    *out = (*p == dir_neg) ? -value : value;
    return true;
}

/* "+CGPSINFO: 1259.432487,N,07732.253059,E,270820,075955.0,965.2,0.0,267.7".
 * While searching it is "+CGPSINFO: ,,,,,,,,". */
static bool gps_parse_info(const char *reply, int32_t *lat, int32_t *lon)
{
    const char *p = strstr(reply, "+CGPSINFO:");
    if (!p) {
        return false;
    }
    p += 10;
    while (*p == ' ') {
        p++;
    }
    if (!gps_parse_coord(p, 'N', 'S', lat)) {
        return false;
    }
    for (int commas = 0; commas < 2; commas++) {
        p = strchr(p, ',');
        if (!p) {
            return false;
        }
        p++;
    }
    if (!gps_parse_coord(p, 'E', 'W', lon)) {
        return false;
    }
    return !(*lat == 0 && *lon == 0) && *lat >= -90000000 && *lat <= 90000000 && *lon >= -180000000 && *lon <= 180000000;
}

/* Metres between two positions. Short-range flat-earth form, plenty for a 50 m check. */
static uint32_t gps_distance_m(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2)
{
    float x = ((float)lat1 + (float)lat2) * 0.5f * 1.745329e-8f; /* Mean latitude, radians */
    float cos_lat = 1.0f - x * x / 2.0f + x * x * x * x / 24.0f;
    float north = (float)(lat2 - lat1) * 0.1111949f;
    float east = (float)(lon2 - lon1) * 0.1111949f * cos_lat;
    float square = north * north + east * east;
    if (square >= 1.0e12f) {
        return 1000000UL; /* Far enough: no need for the root */
    }
    float root = (square > 1.0f) ? square : 1.0f;
    for (int i = 0; i < 24; i++) {
        root = (root + square / root) * 0.5f;
    }
    return (uint32_t)(root + 0.5f);
}
/* GPS_PARSE_END */

/* GPS_TIME_BEGIN */
/* The UTC date and time in a +CGPSINFO reply, written as an HTTP Date line so it goes through the
 * same GMT to IST conversion as the server's. ...,E,270820,075955.0,... */
static bool gps_time_as_date_line(const char *reply, char *out, size_t size)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *p = strstr(reply, "+CGPSINFO:");
    if (!p) {
        return false;
    }
    for (int commas = 0; commas < 4; commas++) {
        p = strchr(p, ',');
        if (!p) {
            return false;
        }
        p++;
    }
    /* ddmmyy,hhmmss */
    for (int i = 0; i < 13; i++) {
        bool ok = (i == 6) ? (p[i] == ',') : (p[i] >= '0' && p[i] <= '9');
        if (!ok) {
            return false;
        }
    }
    int month = (p[2] - '0') * 10 + (p[3] - '0');
    if (month < 1 || month > 12) {
        return false;
    }
    snprintf(out, size, "Date: %.2s %.3s 20%.2s %.2s:%.2s:%.2s GMT", p, &months[(month - 1) * 3], p + 4, p + 7, p + 9, p + 11);
    return true;
}
/* GPS_TIME_END */

/* Same rule as the server's: only a real drift moves the clock, or a clock that isn't valid */
static void __attribute__((noinline)) correct_clock_from_gps(const char *reply)
{
    char date_line[40];
    char gps_ts[16];
    char msg[72];
    telemetry_data_t t;

    if (!gps_time_as_date_line(reply, date_line, sizeof(date_line)) ||
        !intermcu_parse_server_time(date_line, gps_ts, sizeof(gps_ts))) {
        storage_log_debug("RTC", "No usable time in the GPS reply");
        return;
    }

    bool have_clock = intermcu_get_telemetry(&t);
    long drift = have_clock ? intermcu_clock_drift_sec(t.present_time, gps_ts) : -1;
    if (drift >= 0 && drift < clock_drift_limit(false)) {
        snprintf(msg, sizeof(msg), "Clock agrees with GPS, drift %lds", drift);
        storage_log_debug("RTC", msg);
        return;
    }

    snprintf(msg, sizeof(msg), "RTC corrected from GPS: %.14s -> %s (drift %lds)",
             have_clock ? t.present_time : "none", gps_ts, drift);
    storage_log_debug("RTC", msg);
    hal_power_sync_rtc(gps_ts);
    intermcu_update_time(gps_ts);
}

static void gps_turn_off(void)
{
    clear_rx();
    send_raw("AT+CGPS=0\r\n");
    wait_response("+CGPS: 0", 3000, NULL, 0);
    clear_rx();
    send_raw("AT+CVAUXS=0\r\n"); /* Antenna supply off */
    wait_response("OK", 1000, NULL, 0);
}

gps_result_t sim7600_gps_update(const char *today)
{
    char cap[128];
    char line[96];
    int32_t lat = 0, lon = 0;

    storage_log_debug("GPS", "Search starting");
    report_progress("GPS STARTING... ");

    /* Older firmware set CGPSAUTO=1, which starts GPS at every modem power-up */
    clear_rx();
    send_raw("AT+CGPSAUTO=0\r\n");
    wait_response("OK", 1000, NULL, 0);
    /* Supply for an active antenna, as in the TWS firmware */
    clear_rx();
    send_raw("AT+CVAUXV=3050\r\n");
    wait_response("OK", 1000, NULL, 0);
    clear_rx();
    send_raw("AT+CVAUXS=1\r\n");
    wait_response("OK", 1000, NULL, 0);

    cap[0] = '\0';
    clear_rx();
    send_raw("AT+CGPS?\r\n");
    wait_response("OK", 2000, cap, sizeof(cap));
    if (!strstr(cap, "+CGPS: 1")) {
        clear_rx();
        send_raw("AT+CGPS=1\r\n");
        if (!wait_response("OK", 3000, NULL, 0)) {
            clear_rx();
            send_raw("AT+CGPS=1,1\r\n");
            if (!wait_response("OK", 3000, NULL, 0)) {
                storage_log_debug("GPS", "Modem refused AT+CGPS");
                gps_turn_off();
                return GPS_NOT_AVAILABLE;
            }
        }
    }

    /* Two readings in a row that agree: the very first fix after a cold start can be well off */
    bool have_first = false, have_fix = false;
    int32_t first_lat = 0, first_lon = 0;
    uint32_t waited = 0;
    report_progress("GPS SEARCHING...");
    while (waited < GPS_FIX_TIMEOUT_SEC && !have_fix) {
        delay_ms(5000);
        waited += 5;
        cap[0] = '\0';
        clear_rx();
        send_raw("AT+CGPSINFO\r\n");
        if (wait_response("OK", 2000, cap, sizeof(cap)) && gps_parse_info(cap, &lat, &lon)) {
            if (have_first && gps_distance_m(first_lat, first_lon, lat, lon) <= GPS_MOVE_LIMIT_M) {
                have_fix = true;
            }
            first_lat = lat;
            first_lon = lon;
            have_first = true;
        } else {
            have_first = false;
        }
    }
    gps_turn_off();

    if (have_fix) {
        correct_clock_from_gps(cap); /* cap still holds the agreed reading */
    }

    if (!have_fix) {
        snprintf(line, sizeof(line), "No fix after %lus: GPS off", (unsigned long)waited);
        storage_log_debug("GPS", line);
        return GPS_NO_FIX;
    }

    int32_t old_lat = 0, old_lon = 0;
    bool have_old = flash_nvram_get_location(&old_lat, &old_lon);
    uint32_t moved = have_old ? gps_distance_m(old_lat, old_lon, lat, lon) : 0;

    snprintf(line, sizeof(line), "Fix after %lus: %ld,%ld (millionths of a degree), moved %lum", (unsigned long)waited,
             (long)lat, (long)lon, (unsigned long)moved);
    storage_log_debug("GPS", line);

    if (have_old && moved <= GPS_MOVE_LIMIT_M) {
        return GPS_SAME_POSITION; /* Nothing to write */
    }
    if (!flash_nvram_set_location(lat, lon, today)) {
        storage_log_debug("ERROR", "Could not save the GPS position");
    }
    storage_log_debug("GPS", have_old ? "Station moved beyond the limit: position updated" : "First position saved");
    return have_old ? GPS_MOVED : GPS_NEW_POSITION;
}

void sim7600_get_status(modem_status_t *out_status)
{
    if (out_status) {
        memcpy(out_status, &s_modem_status, sizeof(modem_status_t));
    }
}

void sanitize_phone_number(char *dest, const char *src, size_t dest_size)
{
    if (!dest || !src || dest_size == 0) return;
    while (*src == ' ' || *src == '"') src++;

    /* Extract pure digits (and initial '+') */
    char raw[32];
    size_t ri = 0;
    if (*src == '+') {
        raw[ri++] = '+';
        src++;
    }
    while (*src && ri < sizeof(raw) - 1) {
        if (*src >= '0' && *src <= '9') {
            raw[ri++] = *src;
        }
        src++;
    }
    raw[ri] = '\0';

    const char *digits = raw;
    if (digits[0] == '+') digits++;

    /* Remove legacy 1991 or 199 prefixes */
    if (strncmp(digits, "1991", 4) == 0) digits += 4;
    else if (strncmp(digits, "199", 3) == 0) digits += 3;

    /* If it starts with 91 and has 12 digits total -> 10-digit mobile */
    if (strncmp(digits, "91", 2) == 0 && strlen(digits) == 12) {
        digits += 2;
    } else if (digits[0] == '0' && strlen(digits) == 11) {
        digits += 1;
    }

    /* If it is a 10-digit Indian number, prepend +91 */
    if (strlen(digits) == 10) {
        snprintf(dest, dest_size, "+91%s", digits);
    } else if (raw[0] == '+') {
        snprintf(dest, dest_size, "+%s", digits);
    } else {
        snprintf(dest, dest_size, "%s", digits);
    }
}

/* The ESP32's sequence: AT+CMGF, AT+CSCA?, AT+CMGS, the text, 100 ms, then Ctrl+Z */
static bool __attribute__((noinline)) send_sms_to_number(const char *number, const char *message)
{
    char cap[128] = "";

    clear_rx();
    send_raw("ATE0\r\n");
    wait_response("OK", 500, NULL, 0);

    /* 1. Set text mode */
    clear_rx();
    send_raw("AT+CMGF=1\r\n");
    if (!wait_response("OK", 1000, cap, sizeof(cap))) {
        report_progress("CMGF=1 FAIL     ");
        return false;
    }

    /* 3. Query SMS Service Center Address (SMSC) for diagnostics (do not overwrite SIM native SMSC) */
    clear_rx();
    send_raw("AT+CSCA?\r\n");
    if (wait_response("+CSCA:", 1000, cap, sizeof(cap))) {
        char smsc_log[64];
        snprintf(smsc_log, sizeof(smsc_log), "SMSC: %s", cap);
        storage_log_debug("SMS", smsc_log);
    }

    /* 4. Initiate AT+CMGS */
    char sms_cmd[64];
    snprintf(sms_cmd, sizeof(sms_cmd), "AT+CMGS=\"%s\"\r", number);
    clear_rx();
    send_raw(sms_cmd);
    if (!wait_response(">", 5000, cap, sizeof(cap))) {
        /* Cancel input mode if prompt not received */
        const char esc[] = {0x1B, '\0'};
        send_raw(esc);
        wait_response("OK", 1000, NULL, 0);
        char err_log[80];
        snprintf(err_log, sizeof(err_log), "No '>' prompt. Cap: %s", cap);
        storage_log_debug("SMS", err_log);
        if (strstr(cap, "+CMS ERROR:")) {
            char *p = strstr(cap, "+CMS ERROR:");
            char err_txt[17];
            snprintf(err_txt, sizeof(err_txt), "%-16.16s", p + 1);
            report_progress(err_txt);
        } else {
            report_progress("NO '>' PROMPT   ");
        }
        return false;
    }

    /* Wait 50ms before sending payload after '>' prompt */
    delay_ms(50);

    /* 5. Clean message text; Ctrl+Z (0x1A) follows separately */
    char send_buf[300];
    size_t slen = 0;
    while (*message && slen < sizeof(send_buf) - 2) {
        if (*message != 0x1A) {
            send_buf[slen++] = *message;
        }
        message++;
    }
    /* A CR makes the SIM7600 open a new input line (the second ">" in the log), so end on text */
    while (slen > 0 && (send_buf[slen - 1] == '\r' || send_buf[slen - 1] == '\n')) {
        slen--;
    }
    send_buf[slen] = '\0';

    clear_rx();
    UART_Send(UART_MODEM, (uint8_t *)send_buf, slen, BLOCKING);
    delay_ms(100); /* Breather before the transmit surge */
    uint8_t ctrl_z = 0x1A;
    UART_Send(UART_MODEM, &ctrl_z, 1, BLOCKING);

    /* 6. Wait for cellular network SMS acknowledgement */
    /* The ESP32 waits 20s, but the SIM7600 answers nothing else until this ends, so give it longer */
    if (wait_response("+CMGS:", 60000, cap, sizeof(cap))) {
        report_progress("STATUS SENT OK  ");
        char ok_log[80];
        snprintf(ok_log, sizeof(ok_log), "SMS Sent OK: %s", cap);
        storage_log_debug("SMS", ok_log);
        return true;
    } else {
        char fail_log[80];
        snprintf(fail_log, sizeof(fail_log), "SMS Send Failed: %s", cap);
        storage_log_debug("SMS", fail_log);
        if (strstr(cap, "+CMS ERROR:")) {
            char *p = strstr(cap, "+CMS ERROR:");
            char err_txt[17];
            snprintf(err_txt, sizeof(err_txt), "%-16.16s", p + 1);
            report_progress(err_txt);
        } else if (strstr(cap, "ERROR")) {
            report_progress("CMD ERROR       ");
        } else {
            report_progress("NET TIMEOUT 60S ");
        }
        return false;
    }
}

/* After a send with no confirmation: does the modem still answer? ESC leaves text input. */
static bool __attribute__((noinline)) sms_modem_answers(void)
{
    const char esc[] = {0x1B, '\0'};
    clear_rx();
    send_raw(esc);
    wait_response("OK", 3000, NULL, 0);
    delay_ms(500);
    clear_rx();
    send_raw("AT\r\n");
    return wait_response("OK", 3000, NULL, 0);
}

static void sms_log_query(const char *cmd)
{
    clear_rx();
    send_raw(cmd);
    wait_response("OK", 2000, NULL, 0);
}

bool sim7600_send_sms(const char *dest_phone, const char *message)
{
    if (!dest_phone || !message) return false;

    char sanitized[24];
    sanitize_phone_number(sanitized, dest_phone, sizeof(sanitized));

    /* The ESP32 dials the plain 10-digit number. The +91 form is what last worked on this modem. */
    const char *plain = (strncmp(sanitized, "+91", 3) == 0 && strlen(sanitized) == 13) ? (sanitized + 3) : sanitized;

    /* SMS needs more than a data connection: show what the network has granted */
    sms_log_query("AT+CREG?\r\n");
    sms_log_query("AT+CPSI?\r\n");
    sms_log_query("AT+CGSMS?\r\n");

    char line[64];
    snprintf(line, sizeof(line), "Sending to %s", plain);
    storage_log_debug("SMS", line);
    if (send_sms_to_number(plain, message)) {
        return true;
    }
    if (sim7600_is_cancel_requested()) {
        return false;
    }

    /* Second try on automatic network mode: on 4G only, SMS relies on the network carrying it */
    bool answers = sms_modem_answers();
    storage_log_debug("SMS", answers ? "Modem still answers after the failed send" : "Modem silent after the failed send: restarting it");
    report_progress("TRYING AUTO NET ");
    if (!answers) {
        sim7600_power_off();
        if (!sim7600_power_on()) {
            report_progress("MODEM FAIL      ");
            return false;
        }
    }

    clear_rx();
    send_raw("AT+CNMP=2\r\n");
    wait_response("OK", 2000, NULL, 0);

    char reg[48];
    for (int i = 0; i < 15; i++) {
        delay_ms(2000);
        reg[0] = '\0';
        clear_rx();
        send_raw("AT+CREG?\r\n");
        if (wait_response("+CREG:", 1500, reg, sizeof(reg)) && (strstr(reg, ",1") || strstr(reg, ",5"))) {
            break;
        }
    }
    sms_log_query("AT+CPSI?\r\n");

    snprintf(line, sizeof(line), "Retrying as %s on automatic network mode", sanitized);
    storage_log_debug("SMS", line);
    return send_sms_to_number(sanitized, message);
}

bool sim7600_transmit_status_sms(const telemetry_data_t *telem, int csq_dbm)
{
    if (!telem) return false;

    s_cancel_requested = false;
    report_progress("STARTING SMS... ");

    bool we_powered = false;
    /* Ensure modem is powered on and registered */
    if (!s_modem_status.is_registered) {
        storage_log_debug("SMS", "Modem idle, booting SIM7600 for status SMS...");
        if (!sim7600_power_on() || sim7600_is_cancel_requested()) {
            storage_log_debug("SMS", "Failed to boot/register modem for SMS!");
            report_progress(sim7600_is_cancel_requested() ? "SEND CANCELLED  " : "MODEM FAIL      ");
            sim7600_power_off();
            return false;
        }
        we_powered = true;
    }

    if (sim7600_is_cancel_requested()) {
        report_progress("SEND CANCELLED  ");
        if (we_powered) sim7600_power_off();
        return false;
    }

    /* Refresh live CSQ before composing status SMS */
    report_progress("READING CSQ...  ");
    clear_rx();
    send_raw("AT+CSQ\r\n");
    char csq_cap[64];
    if (wait_response("+CSQ:", 1000, csq_cap, sizeof(csq_cap))) {
        int csq = 99, ber = 99;
        char *p = strstr(csq_cap, "+CSQ: ");
        if (p) {
            sscanf(p, "+CSQ: %d,%d", &csq, &ber);
            s_modem_status.csq_val = csq;
            s_modem_status.csq_dbm = (csq == 99) ? -113 : (-113 + 2 * csq);
            csq_dbm = s_modem_status.csq_dbm;
        }
    }

    char csq_step[17];
    snprintf(csq_step, sizeof(csq_step), "CSQ: %d dBm    ", csq_dbm);
    report_progress(csq_step);
    delay_ms(500);

    const char *dest_phone = STATUS_SMS_DEST_NUMBER;
    const char *network_name = STATUS_SMS_NETWORK_NAME;
    const char *stn_type = (UNIT_CFG == KSNDMC_TWS) ? "TWS" : "TRG";
    const char *stn_id = flash_nvram_get_station_id();
    /* The status SMS carries the number without its leading zeros: 001923 goes as 1923 */
    while (stn_id[0] == '0' && stn_id[1] != '\0') {
        stn_id++;
    }

    /* Format timestamp in ISO: YYYY-MM-DDTHH:MM */
    char time_iso[24] = "2026-01-01T00:00";
    if (strlen(telem->present_time) >= 12) {
        snprintf(time_iso, sizeof(time_iso), "%.4s-%.2s-%.2sT%.2s:%.2s",
                 telem->present_time, telem->present_time + 4, telem->present_time + 6,
                 telem->present_time + 8, telem->present_time + 10);
    }

    /* ESP32 flow: the last field is the last record the server got, not the slot in progress */
    char slot_iso[24] = "0000-00-00T00:00";
    const char *ll = flash_nvram_get_last_logged(); /* "DD-MM-YYYY HH:MM" */
    if (strlen(ll) >= 16 && ll[2] == '-' && ll[5] == '-') {
        snprintf(slot_iso, sizeof(slot_iso), "%.4s-%.2s-%.2sT%.5s", ll + 6, ll + 3, ll, ll + 11);
    }

    char calib_date[12] = "0000-00-00";
    #if (UNIT_CFG == KSNDMC_TWS)
    /* TWS has no rain gauge, so field calibration doesn't apply */
    const char *calib_status = "CLB-NA";
    #else
    /* Last field calibration (ESP32 flow): date and result, FAIL if never calibrated */
    const char *calib_result = flash_nvram_get_calib_result();
    if (calib_result[0]) {
        snprintf(calib_date, sizeof(calib_date), "%.10s", calib_result);
    }
    const char *calib_status = strstr(calib_result, "PASS") ? "CLB-OK" : "CLB-FAIL";
    #endif

    const char *sd_status = storage_sd_is_ok() ? "SDC-OK" : "SDC-FAIL";

    /* Exact ESP32 format:
     * <NETWORK>,<STATION_TYPE>,STATUS-F,<STATION_ID>,<YYYY-MM-DDTHH:MM>,<UNIT_VER>,15,<CSQ_DBM>,<BAT1>,<BAT2>,<SOLAR_CHARGING>,<SD_STATUS>,<CALIB_STATUS>,<CALIB_DATE>,<LAST_REC_DATE_TIME>,0\r\n
     */
    char status_msg[256];
    char bat_str[8];
    fmt_fixed(bat_str, sizeof(bat_str), telem->bat1_volt, 4, 1);
    snprintf(status_msg, sizeof(status_msg),
        "%s,%s,STATUS-F,%s,%s,%s,15,%04d,%s,%s,%c,%s,%s,%s,%s,0\r\n",
        network_name,
        stn_type,
        stn_id,
        time_iso,
        FIRMWARE_VERSION,
        csq_dbm,
        bat_str, /* The server shows only this first battery field, so it carries the real voltage */
        bat_str,
        (telem->solar_status == 'C' || telem->solar_volt > 4.0f) ? 'Y' : 'N',
        sd_status,
        calib_status,
        calib_date,
        slot_iso
    );

    report_progress("SENDING SMS...  ");
    storage_log_debug("SMS", status_msg);
    bool ok = sim7600_send_sms(dest_phone, status_msg);

    if (we_powered) {
        delay_ms(1500);
        report_progress("MODEM POWER OFF ");
        sim7600_power_off();
    }
    return ok;
}

/* GPS SMS, as the ESP32's SEND GPS (gprs_health.ino get_lat_long_date_time):
 *   <NETWORK>,<STATION_TYPE>,STAT_AD-<F|C>,<STATION_ID>,<YYYY-MM-DDTHH:MM>,SIM_1,<CSQ_DBM>,<LAT>,<LON>,0.0
 * F = asked from the keypad, C = asked by SMS. The position is the saved one (the GPS job saves a new
 * fix first); none saved yet sends 0.000000 as the ESP32 does. The modem must be on. */
static void fmt_udeg(char *out, size_t size, int32_t udeg)
{
    uint32_t a = (udeg < 0) ? (uint32_t)(-(int64_t)udeg) : (uint32_t)udeg;
    snprintf(out, size, "%s%lu.%06lu", (udeg < 0) ? "-" : "", (unsigned long)(a / 1000000UL), (unsigned long)(a % 1000000UL));
}

/* Builds the message only, and returns: callers send it with sim7600_send_sms() at their own
 * level, so this frame isn't added on top of that deep path (the modem task's stack is short) */
const char *sim7600_build_gps_sms(char kind, const char *present_time)
{
    /* Static: the modem task's stack is the shortest. Both callers hold the modem, so never at once. */
    static char msg[160], lat_s[16], lon_s[16], time_iso[24], csq_cap[64];
    int32_t lat = 0, lon = 0;
    if (!flash_nvram_get_location(&lat, &lon)) {
        lat = 0;
        lon = 0;
    }
    fmt_udeg(lat_s, sizeof(lat_s), lat);
    fmt_udeg(lon_s, sizeof(lon_s), lon);

    const char *stn_id = flash_nvram_get_station_id();
    while (stn_id[0] == '0' && stn_id[1] != '\0') {
        stn_id++;   /* Without leading zeros, as the status SMS */
    }
    strcpy(time_iso, "2026-01-01T00:00");
    if (present_time && strlen(present_time) >= 12) {
        snprintf(time_iso, sizeof(time_iso), "%.4s-%.2s-%.2sT%.2s:%.2s",
                 present_time, present_time + 4, present_time + 6, present_time + 8, present_time + 10);
    }

    clear_rx();
    send_raw("AT+CSQ\r\n");
    if (wait_response("+CSQ:", 2000, csq_cap, sizeof(csq_cap))) {
        int csq = 99;
        char *p = strstr(csq_cap, "+CSQ:");
        if (p && sscanf(p + 5, "%d", &csq) == 1) {
            s_modem_status.csq_dbm = (csq == 99) ? -113 : (-113 + 2 * csq);
        }
    }

    snprintf(msg, sizeof(msg), "%s,%s,STAT_AD-%c,%s,%s,SIM_1,%04d,%s,%s,0.0\r\n",
             STATUS_SMS_NETWORK_NAME, (UNIT_CFG == KSNDMC_TWS) ? "TWS" : "TRG", kind, stn_id, time_iso,
             s_modem_status.csq_dbm, lat_s, lon_s);
    storage_log_debug("SMS", msg);
    return msg;
}

void sim7600_check_incoming_sms(const telemetry_data_t *telemetry)
{
    /* Polled every few seconds with the LCD on: wait for the end of the list, and log only a message */
    clear_rx();
    s_log_quiet = true;
    send_raw("AT+CMGL=\"REC UNREAD\"\r\n");
    bool listed = wait_response("OK", 2000, s_sms_capture, sizeof(s_sms_capture));
    s_log_quiet = false;
    if (listed && strstr(s_sms_capture, "+CMGL: ") != NULL) {
        storage_log_debug("SMS", s_sms_capture);
        char sender_phone[32] = "";
        char *p = strstr(s_sms_capture, "+CMGL: ");
        if (p) {
            /* Extract sender number */
            char *num_start = strchr(p, '\"');
            if (num_start) {
                num_start = strchr(num_start + 1, '\"');
                if (num_start) {
                    num_start = strchr(num_start + 1, '\"');
                    if (num_start) {
                        char *num_end = strchr(num_start + 1, '\"');
                        if (num_end) {
                            size_t nlen = num_end - (num_start + 1);
                            if (nlen < sizeof(sender_phone)) {
                                strncpy(sender_phone, num_start + 1, nlen);
                                sender_phone[nlen] = '\0';
                            }
                        }
                    }
                }
            }
        }

        if (strstr(s_sms_capture, "#STATUS") || strstr(s_sms_capture, "#STAT")) {
            char reply[160];
            char s_bat[8], s_rf15[8], s_cum[8];
            fmt_fixed(s_bat, sizeof(s_bat), telemetry->bat1_volt, 4, 2);
            fmt_fixed(s_rf15, sizeof(s_rf15), telemetry->rf_15m_mm, 4, 1);
            fmt_fixed(s_cum, sizeof(s_cum), telemetry->rf_cum_mm, 4, 1);
            snprintf(reply, sizeof(reply),
                "AIO STN:%s\nTIME:%s\nBAT:%sV\nSIG:%ddBm\nRAIN 15M:%smm\nCUM:%smm\nLAST:%s",
                flash_nvram_get_station_id(), telemetry->present_time, s_bat,
                s_modem_status.csq_dbm, s_rf15, s_cum, flash_nvram_get_last_logged());

            const char *target = (strlen(sender_phone) > 5) ? sender_phone : STATUS_SMS_DEST_NUMBER;
            sim7600_send_sms(target, reply);
        } else if (strstr(s_sms_capture, "#GPS") || strstr(s_sms_capture, "GET_GPS")) {
            /* Old 1768 GET_GPS / ESP32 SEND GPS by SMS: the saved position, at once (a fresh fix can
             * take minutes; MENU > SEND GPS SMS gets one first) */
            const char *target = (strlen(sender_phone) > 5) ? sender_phone : STATUS_SMS_DEST_NUMBER;
            sim7600_send_sms(target, sim7600_build_gps_sms('C', telemetry->present_time));
        } else if (strstr(s_sms_capture, "#TIME=") || strstr(s_sms_capture, "#SET_TIME=")) {
            /* Operator manual time set: #TIME=YYYYMMDDHHMMSS */
            char *tp = strstr(s_sms_capture, "#TIME=");
            if (!tp) tp = strstr(s_sms_capture, "#SET_TIME=");
            if (tp) {
                tp = strchr(tp, '=');
                if (tp && strlen(tp + 1) >= 14) {
                    char ts[16];
                    char reply[80];
                    strncpy(ts, tp + 1, 14);
                    ts[14] = '\0';
                    if (intermcu_ts14_believable(ts)) {
                        hal_power_sync_rtc(ts);
                        intermcu_update_time(ts);
                        snprintf(reply, sizeof(reply), "Clock set by SMS #TIME= to %s", ts);  /* Buffer reused: modem stack */
                        storage_log_debug("RTC", reply);
                        snprintf(reply, sizeof(reply), "AIO STN:%s\nRTC SET OK: %.14s", flash_nvram_get_station_id(), ts);
                    } else {
                        /* The LPC845 takes whatever it is sent: a mistyped SMS must not reach it */
                        storage_log_debug("ERROR", "SMS #TIME= refused: not a valid YYYYMMDDHHMMSS");
                        snprintf(reply, sizeof(reply), "AIO STN:%s\nRTC NOT SET: use #TIME=YYYYMMDDHHMMSS", flash_nvram_get_station_id());
                    }
                    const char *target = (strlen(sender_phone) > 5) ? sender_phone : STATUS_SMS_DEST_NUMBER;
                    sim7600_send_sms(target, reply);
                }
            }
        }

        /* Delete processed read SMS */
        send_raw("AT+CMGD=1,4\r\n");
        wait_response("OK", 1000, NULL, 0);
    }
}
