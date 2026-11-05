#include "intermcu_protocol.h"
#include "user_config.h"
#include "lpc17xx_uart.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define FRAME_MAX_LEN 160

static char s_rx_buffer[FRAME_MAX_LEN];
static uint16_t s_rx_idx = 0;
static bool s_in_frame = false;

static telemetry_data_t s_latest_telemetry = {0};
static float s_daily_cumulative_rain_mm = 0.0f;
static int s_last_reset_day = -1;

/* Hardware Interrupt Handler for Inter-MCU UART3 (P4.28/P4.29) */
void UART3_IRQHandler(void)
{
    uint32_t int_id = (LPC_UART3->IIR & UART_IIR_INTID_MASK);
    if (int_id == UART_IIR_INTID_RDA || int_id == UART_IIR_INTID_CTI) {
        while (LPC_UART3->LSR & UART_LSR_RDR) {
            uint8_t c = (uint8_t)LPC_UART3->RBR;
            intermcu_process_byte(c);
        }
    }
}

void intermcu_protocol_init(void)
{
    memset(&s_latest_telemetry, 0, sizeof(s_latest_telemetry));
    s_rx_idx = 0;
    s_in_frame = false;
}

void intermcu_reset_cumulative_rain(void)
{
    s_daily_cumulative_rain_mm = 0.0f;
    s_latest_telemetry.rf_cum_mm = 0.0f;
    s_latest_telemetry.rf_15m_mm = 0.0f;
    s_latest_telemetry.rf_tips_15m = 0;
}

static void parse_d_packet(const char *frame)
{
    /* Format:
     * $D<time>,<slot>,<rf>,<bat1>,<ltemp>,<lhum>,<lws>,<lwd>,<iws>,<lws_sec>,<ptemp>,<phum>,<pws>,<pwd>,<pws_sec>,<gprs>,<usb>,<ver>,<sc_bat1>,<sc_bat2>,<solar>;
     */
    if (frame[0] != '$' || frame[1] != 'D') return;

    char copy[FRAME_MAX_LEN];
    strncpy(copy, frame + 2, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';

    char *saveptr;
    char *token = strtok_r(copy, ",;", &saveptr);
    int field_idx = 0;

    char present_time[16] = "";
    char slot_time[16] = "";
    int rf_tips = 0;
    int bat1_raw = 0;
    int ltemp_raw = 0, lhum_raw = 0, lws_raw = 0, lwd_raw = 0;
    int iws_raw = 0, lws_sec = 0;
    char gprs_flag = 'N', usb_flag = 'u', ver_code = 'b';
    int sc_bat1_raw = 0, solar_raw = 0;
    char solar_state = 'S';

    while (token != NULL) {
        switch (field_idx) {
            case 0: strncpy(present_time, token, 14); break;
            case 1: strncpy(slot_time, token, 14); break;
            case 2: rf_tips = atoi(token); break;
            case 3: bat1_raw = atoi(token); break;
            case 4: ltemp_raw = atoi(token); break;
            case 5: lhum_raw = atoi(token); break;
            case 6: lws_raw = atoi(token); break;
            case 7: lwd_raw = atoi(token); break;
            case 8: iws_raw = atoi(token); break;
            case 9: lws_sec = atoi(token); break;
            /* fields 10-14 are live readings (ptemp, phum, pws, pwd, pws_sec) */
            case 15: gprs_flag = token[0]; break;
            case 16: usb_flag = token[0]; break;
            case 17: ver_code = token[0]; break;
            case 18: sc_bat1_raw = atoi(token); break;
            case 19: solar_raw = atoi(token); break;
            case 20: solar_state = token[0]; break;
            default: break;
        }
        field_idx++;
        token = strtok_r(NULL, ",;", &saveptr);
    }

    if (field_idx >= 17) {
        if (strlen(present_time) >= 14 && strncmp(present_time, "20", 2) == 0) {
            int p_yr = (present_time[0]-'0')*1000 + (present_time[1]-'0')*100 + (present_time[2]-'0')*10 + (present_time[3]-'0');
            int p_mo = (present_time[4]-'0')*10 + (present_time[5]-'0');
            int p_da = (present_time[6]-'0')*10 + (present_time[7]-'0');
            if (p_yr >= 2024 && p_mo >= 1 && p_mo <= 12 && p_da >= 1 && p_da <= 31) {
                strncpy(s_latest_telemetry.present_time, present_time, 15);
                s_latest_telemetry.present_time[15] = '\0';
            }
        } else if (strlen(s_latest_telemetry.present_time) < 14) {
            strncpy(s_latest_telemetry.present_time, present_time, 15);
            s_latest_telemetry.present_time[15] = '\0';
        }
        strncpy(s_latest_telemetry.slot_time, slot_time, 15);
        s_latest_telemetry.slot_time[15] = '\0';

        s_latest_telemetry.rf_tips_15m = (uint16_t)rf_tips;
        s_latest_telemetry.rf_15m_mm = rf_tips * RF_BUCKET_RESOLUTION;

        /* Daily cumulative rain tracking (reset daily at 08:30 AM or date change) */
        if (strlen(present_time) >= 8) {
            int current_day = (present_time[6] - '0') * 10 + (present_time[7] - '0');
            int current_hour = (present_time[8] - '0') * 10 + (present_time[9] - '0');
            int current_min = (present_time[10] - '0') * 10 + (present_time[11] - '0');

            if (current_day != s_last_reset_day && current_hour == 8 && current_min >= 30) {
                s_daily_cumulative_rain_mm = 0.0f;
                s_last_reset_day = current_day;
            }
        }
        s_daily_cumulative_rain_mm += s_latest_telemetry.rf_15m_mm;
        s_latest_telemetry.rf_cum_mm = s_daily_cumulative_rain_mm;

        /* Battery 1 Voltage (R1=15k, R2=2.2k -> ratio 0.128, 12-bit ADC 0..4095 at 3.3V) */
        float b1 = 0.0f;
        if (sc_bat1_raw > 0) {
            b1 = (((float)sc_bat1_raw * 3.3f / 4095.0f) / 0.128f);
        }
        if (b1 < 2.0f && bat1_raw > 0) {
            b1 = (((float)bat1_raw * 3.3f / 4095.0f) / 0.128f);
        }
        s_latest_telemetry.bat1_volt = b1;

        s_latest_telemetry.temp_c = ltemp_raw / 10.0f;
        s_latest_telemetry.humidity_pct = lhum_raw / 10.0f;
        s_latest_telemetry.wind_speed_avg = lws_raw / 10.0f;
        s_latest_telemetry.wind_dir_deg = (uint16_t)lwd_raw;
        s_latest_telemetry.wind_speed_inst = iws_raw / 10.0f;
        s_latest_telemetry.wind_valid_sec = (uint16_t)lws_sec;

        s_latest_telemetry.gprs_power_flag = gprs_flag;
        s_latest_telemetry.usb_power_flag = usb_flag;
        s_latest_telemetry.lpc845_ver = ver_code;

        /* Solar Voltage (R1=15k, R2=2.2k -> ratio 0.128) */
        float sol = 0.0f;
        if (solar_raw > 0) {
            sol = (((float)solar_raw * 3.3f / 4095.0f) / 0.128f);
        }
        s_latest_telemetry.solar_volt = sol;
        s_latest_telemetry.solar_status = solar_state;
        s_latest_telemetry.is_valid = true;
    }
}

void intermcu_process_byte(uint8_t byte)
{
    if (byte == '$') {
        s_rx_idx = 0;
        s_rx_buffer[s_rx_idx++] = '$';
        s_in_frame = true;
    } else if (s_in_frame) {
        if (s_rx_idx < FRAME_MAX_LEN - 1) {
            s_rx_buffer[s_rx_idx++] = (char)byte;
            if (byte == ';') {
                s_rx_buffer[s_rx_idx] = '\0';
                s_in_frame = false;
                parse_d_packet(s_rx_buffer);
            }
        } else {
            /* Frame overflow, reset */
            s_in_frame = false;
            s_rx_idx = 0;
        }
    }
}

bool intermcu_get_telemetry(telemetry_data_t *out_data)
{
    if (!out_data || !s_latest_telemetry.is_valid) return false;
    memcpy(out_data, &s_latest_telemetry, sizeof(telemetry_data_t));
    return true;
}

void intermcu_update_time(const char *timestamp14)
{
    if (!timestamp14 || strlen(timestamp14) < 14) return;
    strncpy(s_latest_telemetry.present_time, timestamp14, 15);
    s_latest_telemetry.present_time[15] = '\0';
    s_latest_telemetry.is_valid = true;
}

void intermcu_tick_second(void)
{
    if (!s_latest_telemetry.is_valid || strlen(s_latest_telemetry.present_time) < 14) return;

    int yr = 0, mo = 0, da = 0, hr = 0, mi = 0, se = 0;
    char buf[5];

    buf[4] = '\0';
    memcpy(buf, s_latest_telemetry.present_time, 4);
    yr = atoi(buf);

    buf[2] = '\0';
    memcpy(buf, s_latest_telemetry.present_time + 4, 2);
    mo = atoi(buf);
    memcpy(buf, s_latest_telemetry.present_time + 6, 2);
    da = atoi(buf);
    memcpy(buf, s_latest_telemetry.present_time + 8, 2);
    hr = atoi(buf);
    memcpy(buf, s_latest_telemetry.present_time + 10, 2);
    mi = atoi(buf);
    memcpy(buf, s_latest_telemetry.present_time + 12, 2);
    se = atoi(buf);

    if (yr < 2024 || mo < 1 || mo > 12 || da < 1 || da > 31) return;

    se++;
    if (se >= 60) {
        se = 0;
        mi++;
        if (mi >= 60) {
            mi = 0;
            hr++;
            if (hr >= 24) {
                hr = 0;
                da++;
                static const int days_per_month[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
                int leap = ((yr % 4 == 0 && yr % 100 != 0) || (yr % 400 == 0)) ? 1 : 0;
                int mdays = (mo == 2) ? (28 + leap) : days_per_month[mo];
                if (da > mdays) {
                    da = 1;
                    mo++;
                    if (mo > 12) {
                        mo = 1;
                        yr++;
                    }
                }
            }
        }
    }

    snprintf(s_latest_telemetry.present_time, sizeof(s_latest_telemetry.present_time),
             "%04d%02d%02d%02d%02d%02d", yr, mo, da, hr, mi, se);
}

bool intermcu_is_rails_active(void)
{
    /* Check if LPC845 coprocessor has asserted power rails active ('U' for 5V LCD, 'G' for 3.8V GPRS) */
    if (s_latest_telemetry.usb_power_flag == 'U' || s_latest_telemetry.gprs_power_flag == 'G') {
        return true;
    }

    return false;
}

void intermcu_clear_power_flags(void)
{
    s_latest_telemetry.usb_power_flag = 'u';
    s_latest_telemetry.gprs_power_flag = 'g';
}


