/* Host test for sms_cmd.c: cc -I.. sms_cmd_test.c ../sms_cmd.c */
#include "sms_cmd.h"
#include <stdio.h>
#include <string.h>

static int s_fail;
#define T(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); s_fail++; } } while (0)

static sms_cmd_t P(const char *body, const char *want_arg)
{
    char arg[96];
    sms_cmd_t c = sms_cmd_parse(body, arg, sizeof(arg));
    if (want_arg && strcmp(arg, want_arg) != 0) {
        printf("FAIL arg for \"%s\": got \"%s\" want \"%s\"\n", body, arg, want_arg);
        s_fail++;
    }
    return c;
}

int main(void)
{
    /* ESP32 form, any case, with or without = */
    T(P("VARSHA GET_STATUS", "") == SMS_CMD_GET_STATUS);
    T(P("varsha get_gps\r\n", "") == SMS_CMD_GET_GPS);
    T(P("VARSHA SET_STN_ID 1903", "1903") == SMS_CMD_SET_STN_ID);
    T(P("VARSHA SET_STN_ID=001903", "001903") == SMS_CMD_SET_STN_ID);
    T(P("VARSHA SET_STATION_ID 00001923", "00001923") == SMS_CMD_SET_STN_ID);
    T(P("VARSHA SET_TIME 2026-11-13 10:45:00", "2026-11-13 10:45:00") == SMS_CMD_SET_TIME);
    T(P("VARSHA SET_SERVER_2 host=devhlt.spatika.net;port=80;path=/tws_gprs", "host=devhlt.spatika.net;port=80;path=/tws_gprs") == SMS_CMD_SET_SERVER_2);
    T(P("VARSHA SET_SERVER_MODE 2", "2") == SMS_CMD_SET_SERVER_MODE);
    T(P("VARSHA ADD_NUM 9741391102", "9741391102") == SMS_CMD_ADD_NUM);
    T(P("VARSHA GET_CONFIG", "") == SMS_CMD_GET_CONFIG);
    T(P("VARSHA SEND_FTP_DAILY_DATA", NULL) == SMS_CMD_UNKNOWN);
    T(P("VARSHA", NULL) == SMS_CMD_UNKNOWN);
    /* Older LPC form */
    T(P("#STATUS", "") == SMS_CMD_GET_STATUS);
    T(P("#STAT", "") == SMS_CMD_GET_STATUS);
    T(P("#GPS", "") == SMS_CMD_GET_GPS);
    T(P("GET_GPS", "") == SMS_CMD_GET_GPS);
    T(P("#TIME=20261113104500", "20261113104500") == SMS_CMD_SET_TIME);
    T(P("#SET_TIME=20261113104500\r\n", "20261113104500") == SMS_CMD_SET_TIME);
    T(P("#STN_ID=1903", "1903") == SMS_CMD_SET_STN_ID);
    /* Not commands */
    T(P("Hello, recharge offer", NULL) == SMS_CMD_NONE);
    T(P("", NULL) == SMS_CMD_NONE);
    T(P("#STATUSX", NULL) == SMS_CMD_UNKNOWN);   /* Whole words only */

    T(!sms_cmd_needs_office(SMS_CMD_GET_STATUS) && !sms_cmd_needs_office(SMS_CMD_GET_GPS) && !sms_cmd_needs_office(SMS_CMD_GET_CONFIG));
    T(sms_cmd_needs_office(SMS_CMD_SET_TIME) && sms_cmd_needs_office(SMS_CMD_SET_STN_ID) && sms_cmd_needs_office(SMS_CMD_ADD_NUM));

    char n[11];
    T(sms_number_last10("+919980945474", n) && strcmp(n, "9980945474") == 0);
    T(sms_number_last10("09980945474", n) && strcmp(n, "9980945474") == 0);
    T(!sms_number_last10("12345", n));
    T(sms_number_same("+919980945474", "9980945474"));
    T(!sms_number_same("+919980945475", "9980945474"));
    T(!sms_number_same("AIRTEL", "9980945474"));

    char ts[15];
    T(sms_parse_time("20261113104500", ts) && strcmp(ts, "20261113104500") == 0);
    T(sms_parse_time("2026-11-13 10:45:00", ts) && strcmp(ts, "20261113104500") == 0);
    T(!sms_parse_time("2026111310450", ts));
    T(!sms_parse_time("20261313104500", ts));
    T(!sms_parse_time("20261113254500", ts));
    T(!sms_parse_time("2026111310450X", ts));
    T(!sms_parse_time("20001113104500", ts));

    char id[12];
    T(sms_parse_station_id("1903", 6, id, sizeof(id)) && strcmp(id, "001903") == 0);
    T(sms_parse_station_id("001903", 6, id, sizeof(id)) && strcmp(id, "001903") == 0);
    T(sms_parse_station_id("1923", 8, id, sizeof(id)) && strcmp(id, "00001923") == 0);
    T(!sms_parse_station_id("1234567", 6, id, sizeof(id)));
    T(!sms_parse_station_id("000000", 6, id, sizeof(id)));
    T(!sms_parse_station_id("19A3", 6, id, sizeof(id)));
    T(!sms_parse_station_id("", 6, id, sizeof(id)));

    char host[48], path[64], key[20];
    uint16_t port = 0;
    bool off = true;
    T(sms_parse_server2("host=devhlt.spatika.net;port=8085;path=/tws_gprs;key=abc", host, sizeof(host), &port, path, sizeof(path), key, sizeof(key), &off));
    T(!off && strcmp(host, "devhlt.spatika.net") == 0 && port == 8085 && strcmp(path, "/tws_gprs") == 0 && strcmp(key, "abc") == 0);
    T(sms_parse_server2("path=/x;host=1.2.3.4", host, sizeof(host), &port, path, sizeof(path), key, sizeof(key), &off));
    T(port == 80 && key[0] == '\0' && strcmp(host, "1.2.3.4") == 0);
    T(sms_parse_server2("off", host, sizeof(host), &port, path, sizeof(path), key, sizeof(key), &off) && off && host[0] == '\0');
    T(!sms_parse_server2("host=a.b;path=nopath", host, sizeof(host), &port, path, sizeof(path), key, sizeof(key), &off));
    T(!sms_parse_server2("host=a.b;path=/x;port=70000", host, sizeof(host), &port, path, sizeof(path), key, sizeof(key), &off));
    T(!sms_parse_server2("path=/x", host, sizeof(host), &port, path, sizeof(path), key, sizeof(key), &off));
    char longhost[80];
    memset(longhost, 'a', 60); longhost[60] = '\0';
    char arg[120];
    snprintf(arg, sizeof(arg), "host=%s;path=/x", longhost);
    T(!sms_parse_server2(arg, host, sizeof(host), &port, path, sizeof(path), key, sizeof(key), &off));

    char out[200];
    T(sms_payload_with_key("stn_no=001903&rec_time=x&key=climate4pTWS", "NEW", out, sizeof(out)) && strcmp(out, "stn_no=001903&rec_time=x&key=NEW") == 0);
    T(sms_payload_with_key("a=1&key=pse2420&b=2", "K", out, sizeof(out)) && strcmp(out, "a=1&key=K&b=2") == 0);
    T(sms_payload_with_key("{\"StnNo\":\"1\",\"ApiKey\":\"bmsk1234\"}", "zz", out, sizeof(out)) && strcmp(out, "{\"StnNo\":\"1\",\"ApiKey\":\"zz\"}") == 0);
    T(!sms_payload_with_key("no key here", "zz", out, sizeof(out)));
    T(!sms_payload_with_key("a=1&key=x", "zz", out, 5));

    printf(s_fail ? "FAILED: %d\n" : "ALL PASS\n", s_fail);
    return s_fail != 0;
}
