#ifndef TELEMETRY_FORMATTER_H
#define TELEMETRY_FORMATTER_H

#include <stddef.h>

/* Same output as printf("%0<width>.<decimals>f"), rounding halves up. Use this, not %f:
 * %f is not linked in, because it pulls malloc and shared C library state into RTOS tasks. */
void fmt_fixed(char *out, size_t size, float val, int width, int decimals);

/* Pads a station ID to the 6 digits used in records and file names */
void format_station_id(char *out, size_t size, const char *stn_id);

#include "intermcu_protocol.h"
#include <stddef.h>

/* Supported Ingest Endpoints */
typedef enum {
    FORMAT_URL_ENCODED,
    FORMAT_JSON
} payload_format_t;

typedef struct {
    const char *domain;
    const char *ip;
    const char *path;
    uint16_t   port;
    const char *api_key;
    payload_format_t format;
} http_endpoint_t;

/* Formatter Functions */
int format_payload_ksndmc_trg(char *dest, size_t dest_size, const char *stn_id, const telemetry_data_t *data, int csq_dbm);
int format_payload_bihar_trg(char *dest, size_t dest_size, const char *stn_id, const telemetry_data_t *data, int csq_dbm);
int format_payload_ksndmc_tws(char *dest, size_t dest_size, const char *stn_id, const telemetry_data_t *data, int csq_dbm);

/* Endpoint Resolver based on Active Configuration */
const http_endpoint_t* telemetry_get_endpoint(int config_id);

/* SD card record in the ESP32 layout (no line ending), with the rain-day sample number.
 * TRG: NN,YYYY-MM-DD,HH:MM,RAIN15,RAINDAY,SIG,BAT   TWS: NN,YYYY-MM-DD,HH:MM,T,H,WS,WD,SIG,BAT */
int format_sd_record(char *dest, size_t dest_size, const telemetry_data_t *data, int csq_dbm);

#endif /* TELEMETRY_FORMATTER_H */

