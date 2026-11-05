#include "sim7600_driver.h"
#include "hal_power.h"
#include "storage_log.h"
#include "user_config.h"
#include "flash_nvram.h"
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

static void send_raw(const char *cmd)
{
    storage_log_debug("MODEM_TX", cmd);
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
                storage_log_debug("MODEM_RX", s_rx_temp);
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
                storage_log_debug("MODEM_ERR", s_rx_temp);
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
    storage_log_debug("MODEM_TIMEOUT", timeout_msg);
    return false;
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
    if (drift >= 0 && drift < (rejected ? CLOCK_DRIFT_LIMIT_REJECTED_SEC : CLOCK_DRIFT_LIMIT_SEC)) {
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

    if (bsnl) {
        apn = "bsnlnet";
        carrier = "BSNL";
    } else if (strncmp(iccid, "899116", 6) == 0 || strncmp(iccid, "899110", 6) == 0 || strncmp(iccid, "899145", 6) == 0) {
        /* Airtel IoT/M2M SIMs */
        apn = "airteliot.com";
        carrier = "Airtel";
    } else if (strncmp(imsi, "40586", 5) == 0 || strncmp(imsi, "40587", 5) == 0) {
        apn = "jionet";
        carrier = "Jio";
    } else if (strncmp(imsi, "40411", 5) == 0 || strncmp(imsi, "40420", 5) == 0) {
        apn = "www";
        carrier = "Vi";
    } else if (strncmp(imsi, "40445", 5) == 0 || strncmp(imsi, "40552", 5) == 0) {
        carrier = "Airtel";
    }

    if (carrier[0]) {
        strncpy(s_modem_status.carrier, carrier, sizeof(s_modem_status.carrier) - 1);
    }
    return apn;
}

bool sim7600_power_on(void)
{
    if (s_modem_status.is_registered) {
        return true;
    }

    report_progress("MODEM POWER ON ");

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
    for (int i = 0; i < 15; i++) {
        if (sim7600_is_cancel_requested()) return false;
        clear_rx();
        send_raw("AT+CPIN?\r\n");
        if (wait_response("+CPIN: READY", 1000, NULL, 0)) {
            sim_ready = true;
            break;
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
    }

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

    /* 7. Radio Mode Selection (ESP32 flow): start on the mode that last worked (4G by default),
     * then try the other after 5 polls. The last mode is kept on SD; this chip powers off. */
    char rat_saved[8] = "";
    int rat_mode = 38;
    if (storage_state_load(RAT_MODE_FILENAME, rat_saved, sizeof(rat_saved)) && atoi(rat_saved) == 13) {
        rat_mode = 13;
    }
    int rat_at_boot = rat_mode;
    char rat_cmd[20];
    snprintf(rat_cmd, sizeof(rat_cmd), "AT+CNMP=%d\r\n", rat_mode);
    send_raw(rat_cmd);
    wait_response("OK", 1000, NULL, 0);

    /* 8. Registration Polling (check LTE CEREG, GPRS CGREG, and GSM CREG) */
    report_progress("SEARCHING NET...");
    bool registered = false;
    for (int i = 0; i < 25; i++) {
        if (sim7600_is_cancel_requested()) return false;

        if (i == 5) {
            rat_mode = (rat_mode == 38) ? 13 : 38;
            snprintf(rat_cmd, sizeof(rat_cmd), "AT+CNMP=%d\r\n", rat_mode);
            storage_log_debug("MODEM", "No registration after 5 polls, switching radio mode");
            send_raw(rat_cmd);
            wait_response("OK", 1000, NULL, 0);
        }

        /* Check LTE CEREG */
        clear_rx();
        send_raw("AT+CEREG?\r\n");
        if (wait_response("+CEREG: ", 800, capture, sizeof(capture))) {
            if (strstr(capture, ",1") || strstr(capture, ",5")) {
                registered = true;
                storage_log_debug("MODEM", "Registered on LTE (+CEREG: 1/5)");
                break;
            }
        }

        /* Check GPRS CGREG */
        clear_rx();
        send_raw("AT+CGREG?\r\n");
        if (wait_response("+CGREG: ", 800, capture, sizeof(capture))) {
            if (strstr(capture, ",1") || strstr(capture, ",5")) {
                registered = true;
                storage_log_debug("MODEM", "Registered on GPRS (+CGREG: 1/5)");
                break;
            }
        }

        /* Check GSM CREG */
        clear_rx();
        send_raw("AT+CREG?\r\n");
        if (wait_response("+CREG: ", 800, capture, sizeof(capture))) {
            if (strstr(capture, ",1") || strstr(capture, ",5")) {
                registered = true;
                storage_log_debug("MODEM", "Registered on GSM (+CREG: 1/5)");
                break;
            }
        }

        delay_ms(1000);
    }

    if (registered) {
        report_progress("NET REGISTERED  ");
        if (rat_mode != rat_at_boot) {
            snprintf(rat_saved, sizeof(rat_saved), "%02d\r\n", rat_mode);
            storage_state_save(RAT_MODE_FILENAME, rat_saved);
        }
    } else {
        report_progress("NO NETWORK      ");
        storage_log_debug("MODEM", "Network registration timeout!");
    }

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
    send_raw("AT+CIPCLOSE=0\r\n");
    wait_response("OK", 500, NULL, 0);

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

/* The carrier's own view of the data connection: the APN and address really in use */
static void log_bearer_details(void)
{
    clear_rx();
    send_raw("AT+CGCONTRDP=1\r\n");
    wait_response("OK", 3000, NULL, 0);
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

/* ESP32 flow: the SIM's APN first, then the fallback list, then a CGATT reset. The list stops
 * after about 3 minutes so a bad SIM can't use up the slot. If all fail, the send still runs. */
#define BEARER_SEARCH_MAX_TICKS (180000 / portTICK_RATE_MS)

static void __attribute__((noinline)) ensure_bearer(void)
{
    static const char *const fallback_apns[] = {"airteliot.com", "airtelgprs.com", "bsnlnet", "jionet", "bsnlm2m"};

    if (s_bearer_ready) {
        return;
    }

    const char *primary = s_active_apn;
    portTickType start = xTaskGetTickCount();

    s_bearer_ready = primary[0] && activate_apn(primary);

    for (size_t i = 0; i < sizeof(fallback_apns) / sizeof(fallback_apns[0]) && !s_bearer_ready; i++) {
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

    log_bearer_details();
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
    send_raw("AT+CIPCLOSE=0\r\n");
    wait_response("OK", 1000, NULL, 0);
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
        send_raw("AT+CIPCLOSE=0\r\n");
        return false;
    }

    storage_log_debug("TCP", "Streaming HTTP payload...");
    clear_rx();
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
    send_raw("AT+CIPCLOSE=0\r\n");
    wait_response("OK", 1000, NULL, 0);

    return success;
}

/* The modem's own HTTP commands: the ESP32's fallback when a TCP post gets no answer */
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
        storage_log_debug("HTTP", "Server REJECTED the record");
        sync_clock_from_server(s_http_capture, true);
    } else {
        success = true;
        storage_log_debug("HTTP", "HTTP POST Verified OK through modem HTTP commands");
        sync_clock_from_server(s_http_capture, false);
    }

done:
    clear_rx();
    send_raw("AT+HTTPTERM\r\n");
    wait_response("OK", 2000, NULL, 0);
    return success;
}

bool sim7600_http_post(const http_endpoint_t *endpoint, const char *payload, char *date_hdr_out, size_t date_hdr_size)
{
    if (!endpoint || !payload) return false;

    ensure_bearer();

    bool got_reply = false;
    /* TWS6 order: IP, then domain. If the server answered "no", asking again another way
     * won't change that. */
    bool success = http_post_tcp(endpoint, endpoint->ip, payload, date_hdr_out, date_hdr_size, &got_reply);
    if (!success && !got_reply) {
        delay_ms(1000);
        success = http_post_tcp(endpoint, endpoint->domain, payload, date_hdr_out, date_hdr_size, &got_reply);
    }
    if (!success && !got_reply) {
        success = http_post_modem_stack(endpoint, payload);
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
    send_raw("AT+CIPCLOSE=0\r\n");
    wait_response("OK", 1000, NULL, 0);

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
    send_raw("AT+CIPCLOSE=0\r\n");
    wait_response("OK", 1000, NULL, 0);

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
    send_raw("AT+CIPCLOSE=0\r\n");
    wait_response("OK", 1000, NULL, 0);
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

bool sim7600_send_sms(const char *dest_phone, const char *message)
{
    if (!dest_phone || !message) return false;

    char sanitized[24];
    sanitize_phone_number(sanitized, dest_phone, sizeof(sanitized));

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

    /* 2. Character set */
    clear_rx();
    send_raw("AT+CSCS=\"GSM\"\r\n");
    wait_response("OK", 1000, NULL, 0);

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
    snprintf(sms_cmd, sizeof(sms_cmd), "AT+CMGS=\"%s\"\r", sanitized);
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

    /* 5. Build clean message payload terminated by single Ctrl+Z (0x1A) */
    char send_buf[300];
    size_t slen = 0;
    while (*message && slen < sizeof(send_buf) - 2) {
        if (*message != 0x1A) {
            send_buf[slen++] = *message;
        }
        message++;
    }
    send_buf[slen++] = 0x1A; /* Ctrl+Z */
    send_buf[slen] = '\0';

    clear_rx();
    UART_Send(UART_MODEM, (uint8_t *)send_buf, slen, BLOCKING);

    /* 6. Wait for cellular network SMS acknowledgement */
    if (wait_response("+CMGS:", 20000, cap, sizeof(cap))) {
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
            report_progress("NET TIMEOUT 20S ");
        }
        return false;
    }
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

    /* Format timestamp in ISO: YYYY-MM-DDTHH:MM */
    char time_iso[24] = "2026-01-01T00:00";
    if (strlen(telem->present_time) >= 12) {
        snprintf(time_iso, sizeof(time_iso), "%.4s-%.2s-%.2sT%.2s:%.2s",
                 telem->present_time, telem->present_time + 4, telem->present_time + 6,
                 telem->present_time + 8, telem->present_time + 10);
    }

    /* Format slot time in ISO: YYYY-MM-DDTHH:MM */
    char slot_iso[24] = "2026-01-01T00:00";
    if (strlen(telem->slot_time) >= 12) {
        snprintf(slot_iso, sizeof(slot_iso), "%.4s-%.2s-%.2sT%.2s:%.2s",
                 telem->slot_time, telem->slot_time + 4, telem->slot_time + 6,
                 telem->slot_time + 8, telem->slot_time + 10);
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
        "%s,%s,STATUS-F,%s,%s,%s,15,%04d,03.3,%s,%c,%s,%s,%s,%s,0\r\n",
        network_name,
        stn_type,
        stn_id,
        time_iso,
        FIRMWARE_VERSION,
        csq_dbm,
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

void sim7600_check_incoming_sms(const telemetry_data_t *telemetry)
{
    clear_rx();
    send_raw("AT+CMGL=\"REC UNREAD\"\r\n");
    if (wait_response("+CMGL:", 2000, s_sms_capture, sizeof(s_sms_capture))) {
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
                "AIO STN:%s\nTIME:%s\nBAT:%sV\nSIG:%ddBm\nRAIN 15M:%smm\nCUM:%smm",
                flash_nvram_get_station_id(), telemetry->present_time, s_bat,
                s_modem_status.csq_dbm, s_rf15, s_cum);

            const char *target = (strlen(sender_phone) > 5) ? sender_phone : STATUS_SMS_DEST_NUMBER;
            sim7600_send_sms(target, reply);
        } else if (strstr(s_sms_capture, "#TIME=") || strstr(s_sms_capture, "#SET_TIME=")) {
            /* Operator manual time set: #TIME=YYYYMMDDHHMMSS */
            char *tp = strstr(s_sms_capture, "#TIME=");
            if (!tp) tp = strstr(s_sms_capture, "#SET_TIME=");
            if (tp) {
                tp = strchr(tp, '=');
                if (tp && strlen(tp + 1) >= 14) {
                    char ts[16];
                    strncpy(ts, tp + 1, 14);
                    ts[14] = '\0';
                    hal_power_sync_rtc(ts);
                    storage_log_debug("RTC", "DS1338 RTC manually updated via SMS");

                    char reply[80];
                    snprintf(reply, sizeof(reply), "AIO STN:%s\nRTC SET OK: %.14s", flash_nvram_get_station_id(), ts);
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
