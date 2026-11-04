#include "telemetry_formatter.h"
#include "user_config.h"
#include <stdio.h>
#include <string.h>

static const http_endpoint_t s_endpoints[] = {
    [CFG_KSNDMC_TRG] = {
        .domain = "rtdas.ksndmc.net",
        .ip = "117.216.42.181",
        .path = "/trg_gprs/update_data_sit_v3",
        .port = 80,
        .api_key = "pse2420",
        .format = FORMAT_URL_ENCODED
    },
    [CFG_BIHAR_TRG] = {
        .domain = "rtdasbmsk.spatika.net",
        .ip = "164.100.130.199",
        .path = "/Home/UpdateTRGData",
        .port = 8085,
        .api_key = "bmsk1234",
        .format = FORMAT_JSON
    },
    [CFG_KSNDMC_TWS] = {
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
    if (config_id < 0 || config_id > CFG_KSNDMC_TWS) {
        config_id = CFG_KSNDMC_TWS;
    }
    return &s_endpoints[config_id];
}

/* Helper to parse YYYYMMDDHHMMSS into formatted date strings */
static void format_timestamp(const char *in_time, char *date_out, char *time_out)
{
    if (strlen(in_time) >= 12) {
        /* YYYY-MM-DD */
        snprintf(date_out, 11, "%.4s-%.2s-%.2s", in_time, in_time + 4, in_time + 6);
        /* HH:MM */
        snprintf(time_out, 6, "%.2s:%.2s", in_time + 8, in_time + 10);
    } else {
        strcpy(date_out, "2026-01-01");
        strcpy(time_out, "00:00");
    }
}

int format_payload_ksndmc_trg(char *dest, size_t dest_size, const char *stn_id, const telemetry_data_t *data, int csq_dbm)
{
    char date_buf[12], time_buf[8];
    format_timestamp(data->slot_time, date_buf, time_buf);

    return snprintf(dest, dest_size,
        "stn_id=%06s&rec_time=%s,%s&rainfall=%05.2f&signal=%+04d&key=pse2420&bat_volt=%04.1f",
        stn_id,
        date_buf, time_buf,
        data->rf_cum_mm,
        csq_dbm,
        data->bat1_volt
    );
}

int format_payload_bihar_trg(char *dest, size_t dest_size, const char *stn_id, const telemetry_data_t *data, int csq_dbm)
{
    char date_buf[12], time_buf[8];
    format_timestamp(data->slot_time, date_buf, time_buf);

    return snprintf(dest, dest_size,
        "{\"StnNo\":\"%06s\",\"DeviceTime\":\"%s %s:00\",\"RainDP\":\"%05.1f\",\"RainCuml\":\"%05.1f\",\"BatVolt\":\"%04.1f\",\"SigStr\":\"%+04d\",\"ApiKey\":\"bmsk1234\"}",
        stn_id,
        date_buf, time_buf,
        data->rf_15m_mm,
        data->rf_cum_mm,
        data->bat1_volt,
        csq_dbm
    );
}

int format_payload_ksndmc_tws(char *dest, size_t dest_size, const char *stn_id, const telemetry_data_t *data, int csq_dbm)
{
    char date_buf[12], time_buf[8];
    format_timestamp(data->slot_time, date_buf, time_buf);

    return snprintf(dest, dest_size,
        "stn_no=%06s&rec_time=%s,%s&temp=%05.1f&humid=%05.1f&w_speed=%04.1f&w_dir=%d&signal=%+04d&bat_volt=%05.2f&key=climate4pTWS",
        stn_id,
        date_buf, time_buf,
        data->temp_c,
        data->humidity_pct,
        data->wind_speed_avg,
        data->wind_dir_deg,
        csq_dbm,
        data->bat1_volt
    );
}
