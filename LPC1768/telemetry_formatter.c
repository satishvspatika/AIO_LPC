#include "telemetry_formatter.h"
#include "user_config.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

static const http_endpoint_t s_endpoints[] = {
    [KSNDMC_TRG] = {
        .domain = "rtdas.ksndmc.net",
        .ip = "117.216.42.181",
        .path = "/trg_gprs/update_data_sit_v3",
        .port = 80,
        .api_key = "pse2420",
        .format = FORMAT_URL_ENCODED
    },
    [BIHAR_TRG] = {
        .domain = "rtdasbmsk.spatika.net",
        .ip = "164.100.130.199",
        .path = "/Home/UpdateTRGData",
        .port = 8085,
        .api_key = "bmsk1234",
        .format = FORMAT_JSON
    },
    [KSNDMC_TWS] = {
        .domain = "rtdas.ksndmc.net",
        .ip = "117.216.42.181",
        .path = "/tws_gprs/update_tws_data_v3",
        .port = 80,
        .api_key = "climate4pTWS",
        .format = FORMAT_URL_ENCODED
    }
};

const http_endpoint_t* telemetry_get_endpoint(int config_id)
{
    if (config_id < 0 || config_id > KSNDMC_TWS) {
        config_id = KSNDMC_TWS;
    }
    return &s_endpoints[config_id];
}

void fmt_fixed(char *out, size_t size, float val, int width, int decimals)
{
    long scale = 1;
    for (int i = 0; i < decimals; i++) {
        scale *= 10;
    }

    bool negative = (val < 0.0f);
    long scaled = (long)((negative ? -val : val) * scale + 0.5f);
    int int_width = width - decimals - 1 - (negative ? 1 : 0);
    if (int_width < 1) {
        int_width = 1;
    }
    snprintf(out, size, "%s%0*ld.%0*ld", negative ? "-" : "", int_width, scaled / scale, decimals, scaled % scale);
}

/* Station ID as exactly STATION_ID_DIGITS zero-padded digits (Bihar "1923" -> "00001923",
 * KSNDMC "1061" -> "001061"). A longer stored ID (unit retyped from Bihar) drops only leading
 * zeros; if its real digits still don't fit, the last ones are kept. */
static void format_stn_id(char *out, size_t out_size, const char *stn_id)
{
    const int w = STATION_ID_DIGITS;
    if (!out || out_size < (size_t)w + 1) return;
    if (!stn_id || stn_id[0] == '\0') {
        stn_id = STATION_ID_DEFAULT;
    }
    size_t len = strlen(stn_id);
    while (len > (size_t)w && stn_id[0] == '0') {
        stn_id++;
        len--;
    }
    if (len > (size_t)w) {
        stn_id += len - (size_t)w;
        len = (size_t)w;
    }
    int pad = w - (int)len;
    for (int i = 0; i < pad; i++) out[i] = '0';
    memcpy(out + pad, stn_id, len);
    out[w] = '\0';
}

/* Helper to extract and format slot timestamp (YYYY-MM-DD and HH:MM) */
static void extract_slot_timestamp(const telemetry_data_t *data, char *date_out, char *time_out)
{
    const char *ts = NULL;
    bool needs_slot_rounding = false;

    /* 1. Prefer data->slot_time if valid (YYYY >= 2024) */
    if (data && strlen(data->slot_time) >= 12 && strncmp(data->slot_time, "20", 2) == 0 && data->slot_time[2] != '0') {
        ts = data->slot_time;
    }
    /* 2. Otherwise fall back to data->present_time if valid */
    else if (data && strlen(data->present_time) >= 12 && strncmp(data->present_time, "20", 2) == 0 && data->present_time[2] != '0') {
        ts = data->present_time;
        needs_slot_rounding = true;
    }

    if (ts) {
        /* YYYY-MM-DD */
        snprintf(date_out, 11, "%.4s-%.2s-%.2s", ts, ts + 4, ts + 6);
        date_out[10] = '\0';

        if (needs_slot_rounding) {
            /* Round minutes down to 15-minute slot boundary (00, 15, 30, 45) */
            int min = (ts[10] - '0') * 10 + (ts[11] - '0');
            int slot_min = (min / 15) * 15;
            snprintf(time_out, 6, "%.2s:%02d", ts + 8, slot_min);
        } else {
            snprintf(time_out, 6, "%.2s:%.2s", ts + 8, ts + 10);
        }
        time_out[5] = '\0';
    } else {
        strcpy(date_out, "2026-01-01");
        strcpy(time_out, "00:00");
    }
}

/* Deterministic Float Formatting Helpers (immune to toolchain printf float limitations) */

/* Format float as %05.1f (e.g. 0.0 -> "000.0", 24.2 -> "024.2", -5.2 -> "-05.2") */
static void fmt_float_5_1(char *out, float val)
{
    if (val < 0.0f) {
        int total_tenths = (int)((-val) * 10.0f + 0.5f);
        int integer_part = total_tenths / 10;
        int frac_part = total_tenths % 10;
        snprintf(out, 8, "-%02d.%01d", integer_part, frac_part);
    } else {
        int total_tenths = (int)(val * 10.0f + 0.5f);
        int integer_part = total_tenths / 10;
        int frac_part = total_tenths % 10;
        snprintf(out, 8, "%03d.%01d", integer_part, frac_part);
    }
}

/* Format float as %04.1f (e.g. 0.0 -> "00.0", 3.5 -> "03.5") */
static void fmt_float_4_1(char *out, float val)
{
    if (val < 0.0f) val = 0.0f;
    int total_tenths = (int)(val * 10.0f + 0.5f);
    int integer_part = total_tenths / 10;
    int frac_part = total_tenths % 10;
    snprintf(out, 8, "%02d.%01d", integer_part, frac_part);
}

/* Format float as %05.2f (e.g. 0.0 -> "00.00", 4.2 -> "04.20", 0.25 -> "00.25") */
static void fmt_float_5_2(char *out, float val)
{
    if (val < 0.0f) val = 0.0f;
    int total_hundredths = (int)(val * 100.0f + 0.5f);
    int integer_part = total_hundredths / 100;
    int frac_part = total_hundredths % 100;
    snprintf(out, 8, "%02d.%02d", integer_part, frac_part);
}

/* Format uint16 as %03d (e.g. 0 -> "000", 45 -> "045", 344 -> "344") */
static void fmt_uint_3(char *out, uint16_t val)
{
    snprintf(out, 8, "%03u", val % 1000);
}

/* Format signal dBm as %+04d (e.g. -73 -> "-073", -113 -> "-113") */
static void fmt_signal_4(char *out, int val)
{
    if (val < 0) {
        snprintf(out, 8, "-%03d", (-val) % 1000);
    } else {
        snprintf(out, 8, "+%03d", val % 1000);
    }
}

int format_payload_ksndmc_trg(char *dest, size_t dest_size, const char *stn_id, const telemetry_data_t *data, int csq_dbm)
{
    char clean_stn[STATION_ID_MAX + 1];
    format_stn_id(clean_stn, sizeof(clean_stn), stn_id);

    char date_buf[12], time_buf[8];
    extract_slot_timestamp(data, date_buf, time_buf);

    char s_rf[8], s_bat[8], s_sig[8];
    fmt_float_5_2(s_rf,  data ? data->rf_cum_mm : 0.0f);
    fmt_float_4_1(s_bat, data ? data->bat1_volt : 0.0f);
    fmt_signal_4(s_sig,  csq_dbm);

    return snprintf(dest, dest_size,
        "stn_id=%s&rec_time=%s,%s&rainfall=%s&signal=%s&bat_volt=%s&key=pse2420",
        clean_stn,
        date_buf, time_buf,
        s_rf,
        s_sig,
        s_bat
    );
}

int format_payload_bihar_trg(char *dest, size_t dest_size, const char *stn_id, const telemetry_data_t *data, int csq_dbm)
{
    char clean_stn[STATION_ID_MAX + 1];
    format_stn_id(clean_stn, sizeof(clean_stn), stn_id);

    char date_buf[12], time_buf[8];
    extract_slot_timestamp(data, date_buf, time_buf);

    char s_rf15[8], s_rfcum[8], s_bat[8], s_sig[8];
    fmt_float_5_1(s_rf15,  data ? data->rf_15m_mm : 0.0f);
    fmt_float_5_1(s_rfcum, data ? data->rf_cum_mm : 0.0f);
    fmt_float_4_1(s_bat,   data ? data->bat1_volt : 0.0f);
    fmt_signal_4(s_sig,    csq_dbm);

    return snprintf(dest, dest_size,
        "{\"StnNo\":\"%s\",\"DeviceTime\":\"%s %s:00\",\"RainDP\":\"%s\",\"RainCuml\":\"%s\",\"BatVolt\":\"%s\",\"SigStr\":\"%s\",\"ApiKey\":\"bmsk1234\"}",
        clean_stn,
        date_buf, time_buf,
        s_rf15,
        s_rfcum,
        s_bat,
        s_sig
    );
}

int format_payload_ksndmc_tws(char *dest, size_t dest_size, const char *stn_id, const telemetry_data_t *data, int csq_dbm)
{
    char clean_stn[STATION_ID_MAX + 1];
    format_stn_id(clean_stn, sizeof(clean_stn), stn_id);

    char date_buf[12], time_buf[8];
    extract_slot_timestamp(data, date_buf, time_buf);

    char s_temp[8], s_hum[8], s_ws[8], s_wd[8], s_bat[8];
    fmt_float_5_1(s_temp, data ? data->temp_c : 0.0f);
    fmt_float_5_1(s_hum,  data ? data->humidity_pct : 0.0f);
    fmt_float_4_1(s_ws,   data ? data->wind_speed_avg : 0.0f);
    fmt_uint_3(s_wd,      data ? data->wind_dir_deg : 0);
    fmt_float_5_2(s_bat,  data ? data->bat1_volt : 0.0f);

    /* TWS signal is plain %d (-73), unlike the zero-padded TRG field (-073) */
    return snprintf(dest, dest_size,
        "stn_no=%s&rec_time=%s,%s&temp=%s&humid=%s&w_speed=%s&w_dir=%s&signal=%d&bat_volt=%s&key=climate4pTWS",
        clean_stn,
        date_buf, time_buf,
        s_temp,
        s_hum,
        s_ws,
        s_wd,
        csq_dbm,
        s_bat
    );
}

void format_station_id(char *out, size_t size, const char *stn_id)
{
    format_stn_id(out, size, stn_id);
}

int format_sd_record(char *dest, size_t dest_size, const telemetry_data_t *data, int csq_dbm)
{
    char date_buf[12], time_buf[8];
    extract_slot_timestamp(data, date_buf, time_buf);

    /* Rain-day sample number: 08:45 is 00, 08:30 next day is 95 */
    int hour = (time_buf[0] - '0') * 10 + (time_buf[1] - '0');
    int minute = (time_buf[3] - '0') * 10 + (time_buf[4] - '0');
    int sample_no = ((hour * 60 + minute) / 15 + 61) % 96;

    char s_bat[8];
    fmt_fixed(s_bat, sizeof(s_bat), data->bat1_volt, 5, 2);

    #if (UNIT_CFG == KSNDMC_TWS)
    char s_temp[8], s_hum[8], s_ws[8];
    fmt_fixed(s_temp, sizeof(s_temp), data->temp_c, 5, 1);
    fmt_fixed(s_hum, sizeof(s_hum), data->humidity_pct, 5, 1);
    fmt_fixed(s_ws, sizeof(s_ws), data->wind_speed_avg, 4, 1);
    return snprintf(dest, dest_size, "%02d,%s,%s,%s,%s,%s,%03d,%04d,%s",
                    sample_no, date_buf, time_buf, s_temp, s_hum, s_ws, data->wind_dir_deg % 1000, csq_dbm, s_bat);
    #else
    char s_rf15[8], s_cum[8];
    fmt_fixed(s_rf15, sizeof(s_rf15), data->rf_15m_mm, 6, 2);
    fmt_fixed(s_cum, sizeof(s_cum), data->rf_cum_mm, 6, 2);
    return snprintf(dest, dest_size, "%02d,%s,%s,%s,%s,%04d,%s",
                    sample_no, date_buf, time_buf, s_rf15, s_cum, csq_dbm, s_bat);
    #endif
}
