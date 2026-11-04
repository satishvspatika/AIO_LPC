#ifndef SIM7600_DRIVER_H
#define SIM7600_DRIVER_H

#include <stdbool.h>
#include <stdint.h>
#include "telemetry_formatter.h"

typedef struct {
    int csq_val;
    int csq_dbm;
    char ip_addr[20];
    char iccid[24];
    bool is_registered;
    bool is_pdp_active;
} modem_status_t;

void sim7600_init(void);
bool sim7600_power_on(void);
void sim7600_power_off(void);

/* Transmit HTTP Telemetry via Layer-4 TCP Socket */
bool sim7600_http_post(const http_endpoint_t *endpoint, const char *payload, char *date_hdr_out, size_t date_hdr_size);

/* Query live modem status */
void sim7600_get_status(modem_status_t *out_status);

/* SMS Engine */
void sanitize_phone_number(char *dest, const char *src, size_t dest_size);
bool sim7600_send_sms(const char *dest_phone, const char *message);
bool sim7600_transmit_status_sms(const telemetry_data_t *telem, int csq_dbm);
void sim7600_check_incoming_sms(const telemetry_data_t *telemetry);

#endif /* SIM7600_DRIVER_H */

