#ifndef INTERMCU_PROTOCOL_H
#define INTERMCU_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char present_time[16];     /* YYYYMMDDHHMMSS */
    char slot_time[16];        /* YYYYMMDDHHMMSS */
    float live_temp_c;         /* Current sensor readings, updated between 15-minute records */
    float live_humidity_pct;
    float live_wind_speed;
    uint16_t live_wind_dir_deg;
    uint16_t rf_tips_15m;      /* 15-minute rainfall tips count */
    float rf_15m_mm;           /* 15-minute rainfall in mm */
    float rf_cum_mm;           /* 24-hour cumulative rainfall in mm */
    float rf_live_15m_mm;      /* LCD only: rain so far in this slot. Records use rf_15m_mm */
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
    bool is_valid;             /* Frame validity */
} telemetry_data_t;

void intermcu_protocol_init(void);
void intermcu_process_byte(uint8_t byte);

/* The LPC845's newest reply to a firmware update command, if one has come since the last call */
bool intermcu_take_u_reply(char *out, size_t size);
bool intermcu_get_telemetry(telemetry_data_t *out_data);
uint32_t intermcu_get_fat_time(void);
uint32_t intermcu_get_bad_frames(void);
bool intermcu_845_wants_rain_total(void);
void intermcu_update_time(const char *timestamp14);
void intermcu_tick_second(void);
void intermcu_reset_cumulative_rain(void);

/* Days since year 0, for 2024-2099 */
int32_t intermcu_day_number(int yr, int mo, int da);

/* YYYYMMDD of the rain day a record belongs to, from its YYYYMMDDHHMM... time */
bool intermcu_rain_close_date(const char *timestamp, char *date_out, size_t size);

/* Stored 15-minute samples held by the LPC845, for filling slots that were missed.
 * poll returns 0 while waiting, 1 when out_record is filled, -1 if the LPC845 hasn't got it. */
void intermcu_request_record(const char *timestamp14);
int intermcu_poll_record(telemetry_data_t *out_record);

/* SYSTEM the LPC845 firmware was built for, or -1 if its firmware doesn't report it */
int intermcu_get_845_system(void);

/* LPC845 link health, for the log: bytes and frames received, and the latest raw frame */
/* Why the LPC845 last powered this chip ('B' it restarted, 'K' DSP key, 'S' slot, '?' unknown),
 * and why it last restarted (SYSRSTSTAT) */
void intermcu_get_845_wake(char *out_reason, int *out_reset_status);
void intermcu_get_link_stats(uint32_t *out_bytes, uint32_t *out_frames, uint32_t *out_good, char *out_last, size_t size);

/* Server time from an HTTP reply as YYYYMMDDHHMMSS (IST): the body's "tm" field, or else the
 * Date: header converted from GMT. */
bool intermcu_parse_server_time(const char *http_response, char *out_ts14, size_t size);

/* Seconds between two YYYYMMDDHHMMSS times, across midnight; -1 if a date is too far apart
 * or a time is invalid */
long intermcu_clock_drift_sec(const char *a_ts14, const char *b_ts14);
/* YYYYMMDDHHMMSS that the LPC845 can hold: 2024-2079, a real day of that month, 00:00:00-23:59:59.
 * Every time sent by hand (menu, SMS #TIME=) is checked with this first: the LPC845 checks nothing. */
bool intermcu_ts14_believable(const char *ts);
/* CTRL clock: -1 not reported (CTRL before v1.11), 0 its RTC, 1 software clock as the RTC doesn't
 * answer, 2 software clock as the RTC doesn't run */
int intermcu_845_rtc_state(void);

/* Tips counted by the LPC845 during field calibration */
void intermcu_calib_reset(void);
int intermcu_get_calib_tips(void);
bool intermcu_is_rails_active(void);
void intermcu_clear_power_flags(void);

#endif /* INTERMCU_PROTOCOL_H */

