#include "sim7600_driver.h"
#include "hal_power.h"
#include "storage_log.h"
#include "user_config.h"
#include "flash_nvram.h"
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
#define PIN_GPRS_POWER  1       /* P0.1: GPRS power enable (Output HIGH) */

#define RX_BUFFER_SIZE  2048
static char s_rx_buf[RX_BUFFER_SIZE];
static volatile uint16_t s_rx_head = 0;
static volatile uint16_t s_rx_tail = 0;

/* Static buffers to prevent task stack overflows in FreeRTOS */
static char s_rx_temp[RX_BUFFER_SIZE];
static char s_http_req[1024];
static char s_http_capture[1024];
static char s_sms_capture[512];

static modem_status_t s_modem_status = {
    .csq_val = 99,
    .csq_dbm = -113,
    .ip_addr = "0.0.0.0",
    .iccid = "",
    .is_registered = false,
    .is_pdp_active = false
};

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

bool sim7600_is_cancel_requested(void)
{
    return s_cancel_requested;
}

static void report_progress(const char *msg)
{
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

    /* P0.1: GPRS_POWER (GPIO output driven HIGH) */
    pin_cfg.Pinnum = PIN_GPRS_POWER;
    PINSEL_ConfigPin(&pin_cfg);
    LPC_GPIO0->FIOSET = (1UL << PIN_GPRS_POWER); /* HIGH = Power ON */
    LPC_GPIO0->FIODIR |= (1UL << PIN_GPRS_POWER);/* Output */

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
    s_rx_head = 0;
    s_rx_tail = 0;
    memset(s_rx_buf, 0, sizeof(s_rx_buf));
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

            if (strstr(s_rx_temp, expected) != NULL) {
                if (out_capture && capture_size > 0) {
                    strncpy(out_capture, s_rx_temp, capture_size - 1);
                    out_capture[capture_size - 1] = '\0';
                }
                storage_log_debug("MODEM_RX", s_rx_temp);
                return true;
            }
        }
        delay_ms(10);
        elapsed += 10;
    }
    return false;
}

bool sim7600_power_on(void)
{
    report_progress("MODEM POWER ON ");

    /* 1. Turn ON 3.8V rail via LPC845 sentry command ($n2;) */
    hal_power_modem_on();

    /* 2. Select SIM holder J1A (P0.0 LOW) and enable GPRS power circuit (P0.1 HIGH) */
    LPC_GPIO0->FIOCLR = (1UL << PIN_SIM_SEL);       /* Driven LOW = Holder J1A */
    LPC_GPIO0->FIODIR |= (1UL << PIN_SIM_SEL);
    LPC_GPIO0->FIOSET = (1UL << PIN_GPRS_POWER);    /* Driven HIGH = Power ON */
    LPC_GPIO0->FIODIR |= (1UL << PIN_GPRS_POWER);

    /* Allow SIM multiplexer and power supply 2.0s to settle */
    for (int i = 0; i < 20; i++) {
        if (sim7600_is_cancel_requested()) return false;
        delay_ms(100);
    }

    /* 3. Probe modem with "AT" once per second up to 30s or until RDY/OK */
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
    send_raw("AT&D0\r\n");              /* Ignore DTR line to prevent PPP teardown */
    wait_response("OK", 500, NULL, 0);

    /* 5. Check SIM Card Readiness (AT+CPIN?) */
    report_progress("CHECKING SIM... ");
    char capture[256];
    bool sim_ready = false;
    for (int i = 0; i < 10; i++) {
        if (sim7600_is_cancel_requested()) return false;
        clear_rx();
        send_raw("AT+CPIN?\r\n");
        if (wait_response("+CPIN: READY", 1000, NULL, 0)) {
            sim_ready = true;
            break;
        }
        delay_ms(1000);
    }

    /* If SIM not ready after 10s, power-cycle radio via CFUN=0 / CFUN=1 once */
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

    /* Smart APN Chain Lookup:
     * - ICCID prefixes (Airtel IoT/M2M SIMs refuse airtelgprs.com):
     *   899116, 899110, 899145 -> airteliot.com
     * - IMSI MCC-MNC prefixes (first 5 digits):
     *   40445 (Airtel KA), 40552 (Airtel Bihar) -> airtelgprs.com
     *   40471 (BSNL KA), 40475 (BSNL Bihar)    -> bsnlnet
     *   40586, 40587 (Reliance Jio)            -> jionet
     *   40411, 40420 (Vodafone Idea)           -> www
     */
    const char *apn = "airtelgprs.com";
    if (strncmp(s_modem_status.iccid, "899116", 6) == 0 ||
        strncmp(s_modem_status.iccid, "899110", 6) == 0 ||
        strncmp(s_modem_status.iccid, "899145", 6) == 0) {
        apn = "airteliot.com";
    } else if (strncmp(imsi, "40471", 5) == 0 || strncmp(imsi, "40475", 5) == 0) {
        apn = "bsnlnet";
    } else if (strncmp(imsi, "40586", 5) == 0 || strncmp(imsi, "40587", 5) == 0) {
        apn = "jionet";
    } else if (strncmp(imsi, "40411", 5) == 0 || strncmp(imsi, "40420", 5) == 0) {
        apn = "www";
    } else if (strncmp(imsi, "40445", 5) == 0 || strncmp(imsi, "40552", 5) == 0) {
        apn = "airtelgprs.com";
    }

    char log_apn[64];
    snprintf(log_apn, sizeof(log_apn), "APN Selected: %s (IMSI: %s, ICCID: %s)", apn, imsi, s_modem_status.iccid);
    storage_log_debug("APN", log_apn);

    /* Configure PDP Context 1 */
    char apn_cmd[64];
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

    /* Define PDP Context 5 as Network Default empty APN */
    send_raw("AT+CGDCONT=5,\"IP\",\"\"\r\n");
    wait_response("OK", 1000, NULL, 0);

    /* 7. Radio Mode Selection: Automatic RAT selection (CNMP=2 allows seamless LTE/3G/2G attach) */
    send_raw("AT+CNMP=2\r\n");
    wait_response("OK", 1000, NULL, 0);

    /* 8. Registration Polling (check LTE CEREG, GPRS CGREG, and GSM CREG) */
    report_progress("SEARCHING NET...");
    bool registered = false;
    for (int i = 0; i < 25; i++) {
        if (sim7600_is_cancel_requested()) return false;

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

    /* 10. Synchronize time from cellular network base station (NITZ) */
    if (registered) {
        clear_rx();
        send_raw("AT+CTZU=1\r\n"); /* Enable auto network time zone & time */
        wait_response("OK", 1000, NULL, 0);

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
                        storage_log_debug("RTC", "DS1338 RTC synced from cellular tower time");
                    }
                }
            }
        }
    }

    s_modem_status.is_registered = registered;
    return registered;
}

void sim7600_power_off(void)
{
    send_raw("AT+CIPCLOSE=0\r\n");
    wait_response("OK", 1000, NULL, 0);

    send_raw("AT+NETCLOSE\r\n");
    wait_response("OK", 1000, NULL, 0);

    /* Software Power Off */
    send_raw("AT+CPOF\r\n");
    wait_response("OK", 2000, NULL, 0);

    /* Turn OFF P0.1 (GPRS_POWER) */
    LPC_GPIO0->FIOCLR = (1UL << PIN_GPRS_POWER);

    /* Cut 3.8V rail via LPC845 sentry ($f2;) */
    hal_power_modem_off();
    s_modem_status.is_registered = false;
    s_modem_status.is_pdp_active = false;
}

bool sim7600_http_post(const http_endpoint_t *endpoint, const char *payload, char *date_hdr_out, size_t date_hdr_size)
{
    if (!endpoint || !payload) return false;

    char log_msg[128];
    char capture[256];

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
            strncpy(s_modem_status.ip_addr, ip_str, sizeof(s_modem_status.ip_addr) - 1);
        }
    }

    /* Step 3: Close any stale socket 0 connection */
    send_raw("AT+CIPCLOSE=0\r\n");
    wait_response("OK", 1000, NULL, 0);
    delay_ms(100);

    /* Step 4: Open Layer-4 direct TCP socket to endpoint */
    char open_cmd[128];
    snprintf(open_cmd, sizeof(open_cmd), "AT+CIPOPEN=0,\"TCP\",\"%s\",%d\r\n", endpoint->ip, endpoint->port);
    snprintf(log_msg, sizeof(log_msg), "Connecting to %s:%d...", endpoint->ip, endpoint->port);
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
    int req_len = snprintf(s_http_req, sizeof(s_http_req),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        endpoint->path, endpoint->domain, content_type, (int)strlen(payload), payload
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
    if (wait_response("+IPD", 15000, s_http_capture, sizeof(s_http_capture))) {
        storage_log_debug("TCP", "Received server response (+IPD)");
        storage_log_http(s_http_capture);

        if (strstr(s_http_capture, "200 OK") ||
            strstr(s_http_capture, "Success") ||
            strstr(s_http_capture, "SUCCESS") ||
            strstr(s_http_capture, "accepted") ||
            strstr(s_http_capture, "ACCEPTED")) {
            success = true;
            storage_log_debug("TCP", "HTTP POST Verified OK (200 / Success)");

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
            storage_log_debug("TCP", "Response received but no 200 OK / Success matched");
        }
    } else {
        storage_log_debug("TCP", "HTTP Response timeout (no +IPD)!");
    }

    /* Step 8: Close TCP socket */
    send_raw("AT+CIPCLOSE=0\r\n");
    wait_response("OK", 1000, NULL, 0);

    return success;
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

    if (strncmp(src, "+1991", 5) == 0) src += 5;
    else if (strncmp(src, "1991", 4) == 0) src += 4;
    else if (strncmp(src, "+199", 4) == 0) src += 4;
    else if (strncmp(src, "+91", 3) == 0) src += 3;
    else if (*src == '0') src++;

    strncpy(dest, src, dest_size - 1);
    dest[dest_size - 1] = '\0';
}

bool sim7600_send_sms(const char *dest_phone, const char *message)
{
    if (!dest_phone || !message) return false;

    char sanitized[24];
    sanitize_phone_number(sanitized, dest_phone, sizeof(sanitized));

    send_raw("AT+CMGF=1\r\n");
    wait_response("OK", 500, NULL, 0);

    char sms_cmd[48];
    snprintf(sms_cmd, sizeof(sms_cmd), "AT+CMGS=\"%s\"\r\n", sanitized);
    clear_rx();
    send_raw(sms_cmd);
    if (!wait_response(">", 3000, NULL, 0)) return false;

    clear_rx();
    send_raw(message);
    const char ctrl_z[] = {0x1A, '\0'};
    send_raw(ctrl_z);

    return wait_response("+CMGS:", 10000, NULL, 0);
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

    /* Format calib date: YYYY-MM-DD */
    char calib_date[16] = "2026-01-01";
    if (strlen(telem->present_time) >= 8) {
        snprintf(calib_date, sizeof(calib_date), "%.4s-%.2s-%.2s",
                 telem->present_time, telem->present_time + 4, telem->present_time + 6);
    }

    /* Exact ESP32 format:
     * <NETWORK>,<STATION_TYPE>,STATUS-F,<STATION_ID>,<YYYY-MM-DDTHH:MM>,<UNIT_VER>,15,<CSQ_DBM>,<BAT1>,<BAT2>,<SOLAR_CHARGING>,<SD_STATUS>,<CALIB_STATUS>,<CALIB_DATE>,<LAST_REC_DATE_TIME>,0\r\n
     */
    char status_msg[256];
    snprintf(status_msg, sizeof(status_msg),
        "%s,%s,STATUS-F,%s,%s,%s,15,%d,03.3,%04.1f,%c,SDC-OK,CLB-OK,%s,%s,0\r\n",
        network_name,
        stn_type,
        stn_id,
        time_iso,
        LCD_VERSION_STRING,
        csq_dbm,
        telem->bat1_volt,
        (telem->solar_status == 'C' || telem->solar_volt > 4.0f) ? 'Y' : 'N',
        calib_date,
        slot_iso
    );

    report_progress("SENDING SMS...  ");
    storage_log_debug("SMS", status_msg);
    bool ok = sim7600_send_sms(dest_phone, status_msg);
    if (ok) {
        report_progress("STATUS SENT OK  ");
    } else {
        report_progress("STATUS FAILED   ");
    }

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
            snprintf(reply, sizeof(reply),
                "AIO STN:%s\nTIME:%s\nBAT:%04.2fV\nSIG:%ddBm\nRAIN 15M:%04.1fmm\nCUM:%04.1fmm",
                flash_nvram_get_station_id(), telemetry->present_time, telemetry->bat1_volt,
                s_modem_status.csq_dbm, telemetry->rf_15m_mm, telemetry->rf_cum_mm);

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
