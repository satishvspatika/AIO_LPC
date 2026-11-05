#include "intermcu_protocol.h"
#include "user_config.h"
#include "lpc17xx_uart.h"
#include "hal_power.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define FRAME_MAX_LEN 160

static char s_rx_buffer[FRAME_MAX_LEN];
static uint16_t s_rx_idx = 0;
static bool s_in_frame = false;

static telemetry_data_t s_latest_telemetry = {0};

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

int32_t intermcu_day_number(int yr, int mo, int da)
{
    static const int days_before_month[] = {0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    int32_t days = yr * 365 + yr / 4 + days_before_month[mo] + da;
    if ((yr % 4) == 0 && mo <= 2) {
        days--; /* This year's leap day hasn't happened yet */
    }
    return days;
}

/* --- Sensor Scaling (same formulas as the ESP32) --- */
static float raw_to_temp_c(int raw)
{
    return raw > 0 ? ((float)raw / 65536.0f) * 165.0f - 40.0f : 0.0f;
}

static float raw_to_humidity(int raw)
{
    return raw > 0 ? ((float)raw / 65536.0f) * 100.0f : 0.0f;
}

static float pulses_to_wind_speed(int pulses, int seconds)
{
    if (pulses <= 0 || seconds <= 0) {
        return 0.0f;
    }
    float speed = WS_CALIBRATION_FACTOR * (((float)pulses / (float)seconds) / WIND_TEETH_COUNT);
    return speed > 60.0f ? 0.0f : speed; /* Beyond this is noise, as on the ESP32 */
}

static uint16_t adc_to_wind_dir(int adc)
{
    if (adc <= WIND_DIR_ADC_MIN) {
        return 0;
    }
    int degrees = (int)(((float)(adc - WIND_DIR_ADC_MIN) * 360.0f) / (float)(WIND_DIR_ADC_MAX - WIND_DIR_ADC_MIN));
    return (uint16_t)(degrees % 360);
}

static volatile char s_845_wake = '?';
static volatile int s_845_reset_status = -1;

void intermcu_get_845_wake(char *out_reason, int *out_reset_status)
{
    *out_reason = s_845_wake;
    *out_reset_status = s_845_reset_status;
}

static volatile uint32_t s_link_bytes = 0;
static volatile uint32_t s_link_frames = 0;
static volatile uint32_t s_link_good = 0;
static char s_link_last_frame[72];

void intermcu_get_link_stats(uint32_t *out_bytes, uint32_t *out_frames, uint32_t *out_good, char *out_last, size_t size)
{
    *out_bytes = s_link_bytes;
    *out_frames = s_link_frames;
    *out_good = s_link_good;
    strncpy(out_last, s_link_last_frame, size - 1);
    out_last[size - 1] = '\0';
}

static bool ts14_valid(const char *ts)
{
    if (!ts || strlen(ts) < 14 || strncmp(ts, "20", 2) != 0) {
        return false;
    }
    for (int i = 0; i < 14; i++) {
        if (ts[i] < '0' || ts[i] > '9') {
            return false;
        }
    }
    return true;
}

static int digits2(const char *p)
{
    return (p[0] - '0') * 10 + (p[1] - '0');
}

static long ts14_day_seconds(const char *ts)
{
    return digits2(ts + 8) * 3600L + digits2(ts + 10) * 60L + digits2(ts + 12);
}

static int32_t ts14_day_number(const char *ts)
{
    int yr = digits2(ts) * 100 + digits2(ts + 2);
    int mo = digits2(ts + 4);
    if (mo < 1 || mo > 12) {
        return -1;
    }
    return intermcu_day_number(yr, mo, digits2(ts + 6));
}

long intermcu_clock_drift_sec(const char *a_ts14, const char *b_ts14)
{
    if (!ts14_valid(a_ts14) || !ts14_valid(b_ts14)) {
        return -1;
    }
    int32_t day_a = ts14_day_number(a_ts14);
    int32_t day_b = ts14_day_number(b_ts14);
    if (day_a < 0 || day_b < 0) {
        return -1;
    }
    long total = (long)(day_a - day_b) * 86400L + ts14_day_seconds(a_ts14) - ts14_day_seconds(b_ts14);
    if (total < 0) {
        total = -total;
    }
    return (total > 2L * 86400L) ? -1 : total;
}

bool intermcu_parse_server_time(const char *http_response, char *out_ts14, size_t size)
{
    static const int month_days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";

    if (!http_response || !out_ts14 || size < 15) {
        return false;
    }

    /* Body: "tm":"YYMMDDHHMMSS", already IST */
    const char *tm = strstr(http_response, "\"tm\"");
    if (tm) {
        const char *p = strchr(tm + 4, ':');
        if (p) {
            p++;
            while (*p == ' ' || *p == '"') {
                p++;
            }
            bool digits = true;
            for (int i = 0; i < 12; i++) {
                if (p[i] < '0' || p[i] > '9') {
                    digits = false;
                    break;
                }
            }
            if (digits) {
                snprintf(out_ts14, size, "20%.12s", p);
                if (ts14_valid(out_ts14) && ts14_day_number(out_ts14) >= 0) {
                    return true;
                }
            }
        }
    }

    /* Header: "Date: Wed, 01 Apr 2026 07:15:05 GMT" */
    const char *date = strstr(http_response, "Date: ");
    if (!date) {
        return false;
    }
    const char *p = date + 6;
    while (*p && *p != '\r' && *p != '\n' && (*p < '0' || *p > '9')) {
        p++;
    }

    int da = 0, yr = 0, hr = 0, mi = 0, se = 0;
    char mon[4] = "";
    if (sscanf(p, "%d %3s %d %d:%d:%d", &da, mon, &yr, &hr, &mi, &se) != 6) {
        return false;
    }
    const char *found = strstr(months, mon);
    if (!found || mon[0] == '\0' || ((found - months) % 3) != 0) {
        return false;
    }
    int mo = (int)((found - months) / 3) + 1;
    if (yr < 2024 || yr > 2099 || da < 1 || da > 31 || hr > 23 || mi > 59 || se > 59) {
        return false;
    }

    /* GMT to IST: add 5 hours 30 minutes */
    int total_min = hr * 60 + mi + 330;
    if (total_min >= 1440) {
        total_min -= 1440;
        int mdays = (mo == 2 && (yr % 4) == 0) ? 29 : month_days[mo];
        if (++da > mdays) {
            da = 1;
            if (++mo > 12) {
                mo = 1;
                yr++;
            }
        }
    }
    snprintf(out_ts14, size, "%04d%02d%02d%02d%02d%02d", yr, mo, da, total_min / 60, total_min % 60, se);
    return true;
}

/* --- Rain Day ---
 * A rain day runs from the 08:45 slot through the 08:30 slot next day. The always-on LPC845
 * keeps the rainfall and the day's total, and sends the total in every status frame. */
#define RAIN_CLOSE_SLOT 34  /* Slot of the day for 08:30 */

static volatile int s_845_system = -1; /* -1 until a frame from current LPC845 firmware */

int intermcu_get_845_system(void)
{
    return s_845_system;
}

bool intermcu_rain_close_date(const char *timestamp, char *date_out, size_t size)
{
    static const int days_in_month[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int v[6];

    if (!timestamp || strlen(timestamp) < 12) return false;
    for (int i = 0; i < 12; i++) {
        if (timestamp[i] < '0' || timestamp[i] > '9') return false;
    }
    v[0] = (timestamp[0]-'0')*1000 + (timestamp[1]-'0')*100 + (timestamp[2]-'0')*10 + (timestamp[3]-'0');
    for (int i = 1; i < 5; i++) {
        v[i] = (timestamp[2+i*2]-'0')*10 + (timestamp[3+i*2]-'0');
    }

    int yr = v[0], mo = v[1], da = v[2];
    if (yr < 2024 || mo < 1 || mo > 12 || da < 1 || da > 31 || v[3] > 23 || v[4] > 59) return false;

    if ((v[3] * 60 + v[4]) / 15 > RAIN_CLOSE_SLOT) {
        int mdays = (mo == 2 && (yr % 4) == 0) ? 29 : days_in_month[mo];
        if (++da > mdays) {
            da = 1;
            if (++mo > 12) {
                mo = 1;
                yr++;
            }
        }
    }
    snprintf(date_out, size, "%04d%02d%02d", yr, mo, da);
    return true;
}

void intermcu_reset_cumulative_rain(void)
{
    s_latest_telemetry.rf_cum_mm = 0.0f;
    s_latest_telemetry.rf_15m_mm = 0.0f;
    s_latest_telemetry.rf_tips_15m = 0;
    hal_power_rain_reset();
}

/* Reply to the latest stored-sample request */
static volatile int s_record_state = 0; /* 0 waiting, 1 found, -1 not held */
static char s_record_wanted[16];
static int s_record_fields[8];

void intermcu_request_record(const char *timestamp14)
{
    strncpy(s_record_wanted, timestamp14, sizeof(s_record_wanted) - 1);
    s_record_wanted[sizeof(s_record_wanted) - 1] = '\0';
    s_record_state = 0;
    hal_power_request_record(timestamp14);
}

static float battery_adc_to_volts(int raw)
{
    /* Battery divider: R1=15k, R2=2.2k -> ratio 0.128, 12-bit ADC 0..4095 at 3.3V */
    return ((float)raw * 3.3f / 4095.0f) / 0.128f;
}

int intermcu_poll_record(telemetry_data_t *out_record)
{
    if (s_record_state != 1) {
        return s_record_state;
    }

    strncpy(out_record->slot_time, s_record_wanted, sizeof(out_record->slot_time) - 1);
    out_record->slot_time[sizeof(out_record->slot_time) - 1] = '\0';
    out_record->rf_tips_15m = (uint16_t)s_record_fields[0];
    out_record->rf_15m_mm = s_record_fields[0] * RF_BUCKET_RESOLUTION;
    out_record->bat1_volt = battery_adc_to_volts(s_record_fields[1]);
    out_record->temp_c = raw_to_temp_c(s_record_fields[2]);
    out_record->humidity_pct = raw_to_humidity(s_record_fields[3]);
    out_record->wind_speed_avg = pulses_to_wind_speed(s_record_fields[4], s_record_fields[6]);
    out_record->wind_dir_deg = adc_to_wind_dir(s_record_fields[5]);
    out_record->rf_cum_mm = s_record_fields[7] * RF_BUCKET_RESOLUTION;
    return 1;
}

/* $G<Y|N>,<present>,<requested>[,<rf>,<bat>,<temp>,<hum>,<ws>,<wd>,<ws_sec>,<rain_day_tips>]; */
static void parse_g_packet(const char *frame)
{
    char copy[FRAME_MAX_LEN];
    strncpy(copy, frame + 2, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';

    char *saveptr;
    char *token = strtok_r(copy, ",;", &saveptr);
    char status = 'N';
    bool matches = false;
    int values[8];
    int value_count = 0;
    int field_idx = 0;

    while (token != NULL) {
        if (field_idx == 0) {
            status = token[0];
        } else if (field_idx == 2) {
            matches = (strncmp(token, s_record_wanted, 12) == 0);
        } else if (field_idx >= 3 && value_count < 8) {
            values[value_count++] = atoi(token);
        }
        field_idx++;
        token = strtok_r(NULL, ",;", &saveptr);
    }

    if (!matches) {
        return; /* Reply to an older request */
    }
    if (status == 'Y' && value_count == 8) {
        memcpy(s_record_fields, values, sizeof(s_record_fields));
        s_record_state = 1;
    } else {
        s_record_state = -1;
    }
}

static volatile int s_calib_tips = 0;

void intermcu_calib_reset(void)
{
    s_calib_tips = 0;
}

int intermcu_get_calib_tips(void)
{
    return s_calib_tips;
}

/* $C<present>,<calib_tips>,<bat1>,<bat2>; */
static void parse_c_packet(const char *frame)
{
    const char *comma = strchr(frame, ',');
    if (comma) {
        s_calib_tips = atoi(comma + 1);
    }
}

static void parse_d_packet(const char *frame)
{
    /* Format:
     * $D<time>,<slot>,<rf>,<bat1>,<ltemp>,<lhum>,<lws>,<lwd>,<iws>,<lws_sec>,<ptemp>,<phum>,<pws>,<pwd>,<pws_sec>,<gprs>,<usb>,<ver>,<sc_bat1>,<sc_bat2>,<solar>,<rain_day_tips>,<system>;
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
    int ptemp_raw = 0, phum_raw = 0, pws_raw = 0, pwd_raw = 0, pws_sec = 0;
    char wake_reason = '?';
    int reset_status = -1;
    char gprs_flag = 'N', usb_flag = 'u', ver_code = 'b';
    int sc_bat1_raw = 0, solar_raw = 0;
    char solar_state = 'S';
    int rain_day_tips = 0;
    int lpc845_system = -1;

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
            case 10: ptemp_raw = atoi(token); break;
            case 11: phum_raw = atoi(token); break;
            case 12: pws_raw = atoi(token); break;
            case 13: pwd_raw = atoi(token); break;
            case 14: pws_sec = atoi(token); break;
            case 15: gprs_flag = token[0]; break;
            case 16: usb_flag = token[0]; break;
            case 17: ver_code = token[0]; break;
            case 18: sc_bat1_raw = atoi(token); break;
            case 19: solar_raw = atoi(token); break;
            case 20: solar_state = token[0]; break;
            case 21: rain_day_tips = atoi(token); break;
            case 22: lpc845_system = atoi(token); break;
            case 23: wake_reason = token[0]; break;
            case 24: reset_status = atoi(token); break;
            default: break;
        }
        field_idx++;
        token = strtok_r(NULL, ",;", &saveptr);
    }

    if (field_idx >= 17) {
        s_link_good++;
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

        if (field_idx >= 23) {
            s_845_system = lpc845_system;
            s_845_wake = wake_reason;
            s_845_reset_status = reset_status;
            s_latest_telemetry.rf_cum_mm = rain_day_tips * RF_BUCKET_RESOLUTION;
        }

        /* Battery 1 Voltage (R1=15k, R2=2.2k -> ratio 0.128, 12-bit ADC 0..4095 at 3.3V) */
        float b1 = 0.0f;
        if (sc_bat1_raw > 0) {
            b1 = (((float)sc_bat1_raw * 3.3f / 4095.0f) / 0.128f);
        }
        if (b1 < 2.0f && bat1_raw > 0) {
            b1 = (((float)bat1_raw * 3.3f / 4095.0f) / 0.128f);
        }
        s_latest_telemetry.bat1_volt = b1;

        s_latest_telemetry.temp_c = raw_to_temp_c(ltemp_raw);
        s_latest_telemetry.humidity_pct = raw_to_humidity(lhum_raw);
        s_latest_telemetry.wind_speed_avg = pulses_to_wind_speed(lws_raw, lws_sec);
        s_latest_telemetry.wind_dir_deg = adc_to_wind_dir(lwd_raw);
        s_latest_telemetry.wind_speed_inst = pulses_to_wind_speed(iws_raw, 1);
        s_latest_telemetry.wind_valid_sec = (uint16_t)lws_sec;

        s_latest_telemetry.live_temp_c = raw_to_temp_c(ptemp_raw);
        s_latest_telemetry.live_humidity_pct = raw_to_humidity(phum_raw);
        s_latest_telemetry.live_wind_speed = pulses_to_wind_speed(pws_raw, pws_sec);
        s_latest_telemetry.live_wind_dir_deg = adc_to_wind_dir(pwd_raw);

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
    s_link_bytes++;
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
                s_link_frames++;
                strncpy(s_link_last_frame, s_rx_buffer, sizeof(s_link_last_frame) - 1);
                if (s_rx_buffer[1] == 'G') {
                    parse_g_packet(s_rx_buffer);
                } else if (s_rx_buffer[1] == 'C') {
                    parse_c_packet(s_rx_buffer);
                } else {
                    parse_d_packet(s_rx_buffer);
                }
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


