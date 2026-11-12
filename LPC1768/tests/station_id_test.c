/* Station ID padding per station type (v6.9.32: Bihar 8 digits, KSNDMC 6), as sent to the server
 * and used in DATA/ file names. Build once per type:
 *   cc -std=gnu11 -DUNIT_CFG=1 -I.. -o sid ../tests/station_id_test.c ../telemetry_formatter.c && ./sid */
#include <stdio.h>
#include <string.h>
#include "user_config.h"
#include "telemetry_formatter.h"

static int s_fail = 0;

static void check(const char *stored, const char *want)
{
    char out[STATION_ID_MAX + 1];
    memset(out, 'X', sizeof(out));
    format_station_id(out, sizeof(out), stored);
    if (strcmp(out, want) != 0) {
        printf("FAIL type %d: \"%s\" -> \"%s\", want \"%s\"\n", UNIT_CFG, stored, out, want);
        s_fail++;
    }
}

static void check_payload(const char *stored, const char *key_and_id)
{
    char payload[400];
    telemetry_data_t d;
    memset(&d, 0, sizeof(d));
#if (UNIT_CFG == KSNDMC_TRG)
    format_payload_ksndmc_trg(payload, sizeof(payload), stored, &d, -73);
#elif (UNIT_CFG == BIHAR_TRG)
    format_payload_bihar_trg(payload, sizeof(payload), stored, &d, -73);
#else
    format_payload_ksndmc_tws(payload, sizeof(payload), stored, &d, -73);
#endif
    if (!strstr(payload, key_and_id)) {
        printf("FAIL type %d payload: \"%s\" has no %s\n", UNIT_CFG, payload, key_and_id);
        s_fail++;
    }
}

int main(void)
{
#if (UNIT_CFG == BIHAR_TRG)
    check("1923", "00001923");
    check("001923", "00001923");          /* Stored by v6.9.31 and earlier */
    check("00001923", "00001923");
    check("19230001", "19230001");
    check("", "00001061");                /* Build default */
    check_payload("001923", "\"StnNo\":\"00001923\"");
    check_payload("19230001", "\"StnNo\":\"19230001\"");
#else
    check("1922", "001922");
    check("001922", "001922");
    check("00001922", "001922");          /* Unit retyped from Bihar */
    check("12345678", "345678");          /* Too long: the last 6 */
    check("", "001061");
#if (UNIT_CFG == KSNDMC_TRG)
    check_payload("1922", "stn_id=001922&");
#else
    check_payload("1922", "stn_no=001922&");
#endif
#endif
    if (s_fail) {
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
