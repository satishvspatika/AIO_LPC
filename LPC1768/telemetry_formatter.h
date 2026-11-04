#ifndef TELEMETRY_FORMATTER_H
#define TELEMETRY_FORMATTER_H

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

#endif /* TELEMETRY_FORMATTER_H */

