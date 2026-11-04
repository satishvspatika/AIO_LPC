#include "sim7600_driver.h"
#include "hal_power.h"
#include "storage_log.h"
#include "user_config.h"
#include "lpc17xx_uart.h"
#include "lpc17xx_pinsel.h"
#include "lpc17xx_gpio.h"
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
    for (uint32_t i = 0; i < ms; i++) {
        volatile uint32_t count = 25000;
        while (count--) { __asm volatile("nop"); }
    }
}

void UART2_IRQHandler(void)
{
    uint32_t int_id = (UART_MODEM->IIR & UART_IIR_INTID_MASK);
    if (int_id == UART_IIR_INTID_RDA || int_id == UART_IIR_INTID_CTI) {
        while (UART_CheckBusy(UART_MODEM) || (LPC_UART2->LSR & UART_LSR_RDR)) {
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
    /* 1. Turn ON 3.8V rail via LPC845 sentry command ($n2;) */
    hal_power_modem_on();

    /* 2. Select SIM holder J1A (P0.0 LOW) and enable GPRS power circuit (P0.1 HIGH) */
    LPC_GPIO0->FIOCLR = (1UL << PIN_SIM_SEL);       /* Driven LOW = Holder J1A */
    LPC_GPIO0->FIODIR |= (1UL << PIN_SIM_SEL);
    LPC_GPIO0->FIOSET = (1UL << PIN_GPRS_POWER);    /* Driven HIGH = Power ON */
    LPC_GPIO0->FIODIR |= (1UL << PIN_GPRS_POWER);

    /* Allow SIM multiplexer and power supply 2.0s to settle */
    delay_ms(2000);

    /* 3. Probe modem with "AT" once per second up to 30s or until RDY/OK */
    clear_rx();
    bool modem_ready = false;
    for (int i = 0; i < 30; i++) {
        send_raw("AT\r\n");
        if (wait_response("OK", 800, NULL, 0) || wait_response("RDY", 200, NULL, 0)) {
            modem_ready = true;
            break;
        }
        delay_ms(200);
    }

    if (!modem_ready) {
        storage_log_debug("MODEM", "Modem unresponsive to AT probes");
        return false;
    }

    /* 4. Basic modem synchronization */
    send_raw("ATE0\r\n");               /* Echo OFF */
    wait_response("OK", 500, NULL, 0);
    send_raw("AT+CMEE=2\r\n");          /* Verbose error reporting */
    wait_response("OK", 500, NULL, 0);
    send_raw("AT+CGEREP=0\r\n");        /* Silence unsolicited packet domain reports */
    wait_response("OK", 500, NULL, 0);

    /* 5. Check SIM Card Readiness (AT+CPIN?) */
    char capture[256];
    bool sim_ready = false;
    for (int i = 0; i < 10; i++) {
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

    /* Also define PDP Context 5 as Network Default empty APN */
    send_raw("AT+CGDCONT=5,\"IP\",\"\"\r\n");
    wait_response("OK", 1000, NULL, 0);

    /* 7. Radio Mode Selection: 4G LTE-first (CNMP=38) with automatic 2G fallback (CNMP=13) */
    send_raw("AT+CNMP=38\r\n");
    wait_response("OK", 1000, NULL, 0);

    /* 8. Registration Polling (check both LTE CEREG and GSM/GPRS CGREG) */
    bool registered = false;
    for (int i = 0; i < 20; i++) {
        clear_rx();
        send_raw("AT+CEREG?\r\n");
        if (wait_response("+CEREG: ", 1000, capture, sizeof(capture))) {
            if (strstr(capture, ",1") || strstr(capture, ",5")) {
                registered = true;
                break;
            }
        }
        clear_rx();
        send_raw("AT+CGREG?\r\n");
        if (wait_response("+CGREG: ", 1000, capture, sizeof(capture))) {
            if (strstr(capture, ",1") || strstr(capture, ",5")) {
                registered = true;
                break;
            }
        }
        delay_ms(1000);
    }

    if (!registered) {
        storage_log_debug("MODEM", "LTE registration pending, falling back to 2G/GSM (CNMP=13)");
        send_raw("AT+CNMP=13\r\n");
        wait_response("OK", 1000, NULL, 0);
        delay_ms(2000);

        for (int i = 0; i < 15; i++) {
            clear_rx();
            send_raw("AT+CGREG?\r\n");
            if (wait_response("+CGREG: ", 1000, capture, sizeof(capture))) {
                if (strstr(capture, ",1") || strstr(capture, ",5")) {
                    registered = true;
                    break;
                }
            }
            delay_ms(1000);
        }
    }

    /* 9. Read Signal Quality (AT+CSQ) */
    clear_rx();
    send_raw("AT+CSQ\r\n");
    if (wait_response("+CSQ:", 1000, capture, sizeof(capture))) {
        int csq = 99, ber = 99;
        char *p = strstr(capture, "+CSQ: ");
        if (p) {
            sscanf(p, "+CSQ: %d,%d", &csq, &ber);
            s_modem_status.csq_val = csq;
            s_modem_status.csq_dbm = (csq == 99) ? -113 : (-113 + 2 * csq);
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

    /* Open network */
    clear_rx();
    send_raw("AT+NETOPEN\r\n");
    wait_response("OK", 3000, NULL, 0);

    /* Open Layer-4 direct TCP socket */
    char open_cmd[128];
    snprintf(open_cmd, sizeof(open_cmd), "AT+CIPOPEN=0,\"TCP\",\"%s\",%d\r\n", endpoint->ip, endpoint->port);
    clear_rx();
    send_raw(open_cmd);
    if (!wait_response("+CIPOPEN: 0,0", 15000, NULL, 0)) {
        storage_log_debug("TCP", "CIPOPEN failed!");
        return false;
    }

    /* Sockets settling delay */
    delay_ms(150);

    /* Build HTTP Request Headers */
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

    /* Send Data */
    char send_cmd[32];
    snprintf(send_cmd, sizeof(send_cmd), "AT+CIPSEND=0,%d\r\n", req_len);
    clear_rx();
    send_raw(send_cmd);
    if (!wait_response(">", 3000, NULL, 0)) {
        storage_log_debug("TCP", "No '>' prompt!");
        send_raw("AT+CIPCLOSE=0\r\n");
        return false;
    }

    clear_rx();
    send_raw(s_http_req);

    /* Wait for HTTP Response */
    bool success = false;
    if (wait_response("+IPD", 10000, s_http_capture, sizeof(s_http_capture))) {
        if (strstr(s_http_capture, "200 OK") || strstr(s_http_capture, "Success") || strstr(s_http_capture, "accepted")) {
            success = true;
            storage_log_http(s_http_capture);

            /* Extract server Date: header */
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
        }
    }

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
                STATION_ID_DEFAULT, telemetry->present_time, telemetry->bat1_volt,
                s_modem_status.csq_dbm, telemetry->rf_15m_mm, telemetry->rf_cum_mm);

            const char *target = (strlen(sender_phone) > 5) ? sender_phone :
                ((ACTIVE_CONFIG == CFG_BIHAR_TRG) ? "9741391102" : "9980945474");
            sim7600_send_sms(target, reply);
        }

        /* Delete processed read SMS */
        send_raw("AT+CMGD=1,4\r\n");
        wait_response("OK", 1000, NULL, 0);
    }
}
