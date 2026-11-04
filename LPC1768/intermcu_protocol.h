#ifndef INTERMCU_PROTOCOL_H
#define INTERMCU_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    char present_time[16];     /* YYYYMMDDHHMMSS */
    char slot_time[16];        /* YYYYMMDDHHMMSS */
    uint16_t rf_tips_15m;      /* 15-minute rainfall tips count */
    float rf_15m_mm;           /* 15-minute rainfall in mm */
    float rf_cum_mm;           /* 24-hour cumulative rainfall in mm */
    float bat1_volt;           /* Battery 1 voltage (V) */
    float temp_c;              /* Ambient temperature (deg C) */
    float humidity_pct;        /* Relative humidity (%) */
    float wind_speed_avg;      /* 15-min rolling average wind speed (m/s) */
    uint16_t wind_dir_deg;     /* Wind direction (0-359 degrees) */
    float wind_speed_inst;     /* Instantaneous wind speed (m/s) */
    uint16_t wind_valid_sec;   /* Valid sampling seconds in 15m (up to 900s) */
    char gprs_power_flag;      /* GPRS rail flag ('Y'/'N' or 'G'/'g') */
    char usb_power_flag;       /* 5V LCD rail flag ('U' = ON, 'u' = OFF) */
    char lpc845_ver;           /* Coprocessor firmware version */
    float solar_volt;          /* Solar panel voltage (V) */
    char solar_status;         /* 'C' = Charging, 'S' = Standby */
    bool is_valid;             /* Frame validity */
} telemetry_data_t;

void intermcu_protocol_init(void);
void intermcu_process_byte(uint8_t byte);
bool intermcu_get_telemetry(telemetry_data_t *out_data);
void intermcu_update_time(const char *timestamp14);
void intermcu_reset_cumulative_rain(void);

#endif /* INTERMCU_PROTOCOL_H */

