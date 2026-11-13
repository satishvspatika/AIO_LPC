#include "sms_cmd.h"
#include <string.h>
#include <stdlib.h>

static char up(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Case-blind search: office staff type in either case */
static const char *find_nocase(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && up(hay[i]) == up(needle[i])) {
            i++;
        }
        if (i == n) {
            return hay;
        }
    }
    return NULL;
}

typedef struct {
    const char *name;
    sms_cmd_t cmd;
} cmd_name_t;

/* Longer names before their prefixes (SET_SERVER_2 before SERVER_2, STATUS before STAT) */
static const cmd_name_t s_names[] = {
    { "GET_STATUS",      SMS_CMD_GET_STATUS },
    { "GET_HEALTH",      SMS_CMD_GET_STATUS },
    { "STATUS",          SMS_CMD_GET_STATUS },
    { "STAT",            SMS_CMD_GET_STATUS },
    { "GET_GPS",         SMS_CMD_GET_GPS },
    { "GPS",             SMS_CMD_GET_GPS },
    { "GET_CONFIG",      SMS_CMD_GET_CONFIG },
    { "CONFIG",          SMS_CMD_GET_CONFIG },
    { "GET_NUM",         SMS_CMD_GET_NUM },
    { "SET_TIME",        SMS_CMD_SET_TIME },
    { "TIME",            SMS_CMD_SET_TIME },
    { "SET_STATION_ID",  SMS_CMD_SET_STN_ID },
    { "SET_STN_ID",      SMS_CMD_SET_STN_ID },
    { "STN_ID",          SMS_CMD_SET_STN_ID },
    { "SET_SERVER_2",    SMS_CMD_SET_SERVER_2 },
    { "SERVER_2",        SMS_CMD_SET_SERVER_2 },
    { "SET_SERVER_MODE", SMS_CMD_SET_SERVER_MODE },
    { "SERVER_MODE",     SMS_CMD_SET_SERVER_MODE },
    { "ADD_NUM",         SMS_CMD_ADD_NUM },
    { "DEL_NUM",         SMS_CMD_DEL_NUM },
    { "STOP_SENDING",    SMS_CMD_STOP },
    { "STOP",            SMS_CMD_STOP },
    { "RESUME_SENDING",  SMS_CMD_RESUME },
    { "RESUME",          SMS_CMD_RESUME },
    { "ERASE_DATA",      SMS_CMD_ERASE_DATA },
};

/* The command word at p (letters, digits, _), matched whole against the table */
static sms_cmd_t match_word(const char *p, const char **after)
{
    size_t n = 0;
    while ((p[n] >= 'A' && p[n] <= 'Z') || (p[n] >= 'a' && p[n] <= 'z') || (p[n] >= '0' && p[n] <= '9') || p[n] == '_') {
        n++;
    }
    *after = p + n;
    for (size_t i = 0; i < sizeof(s_names) / sizeof(s_names[0]); i++) {
        size_t m = strlen(s_names[i].name);
        if (m == n) {
            size_t k = 0;
            while (k < n && up(p[k]) == s_names[i].name[k]) {
                k++;
            }
            if (k == n) {
                return s_names[i].cmd;
            }
        }
    }
    return SMS_CMD_UNKNOWN;
}

static void take_arg(const char *p, char *arg, size_t arg_size)
{
    while (is_space(*p) || *p == '=' || *p == ':') {
        p++;
    }
    size_t n = 0;
    while (p[n] && p[n] != '\r' && p[n] != '\n' && n + 1 < arg_size) {
        arg[n] = p[n];
        n++;
    }
    while (n > 0 && is_space(arg[n - 1])) {
        n--;
    }
    arg[n] = '\0';
}

sms_cmd_t sms_cmd_parse(const char *body, char *arg, size_t arg_size)
{
    if (arg_size > 0) {
        arg[0] = '\0';
    }
    if (!body || !arg || arg_size == 0) {
        return SMS_CMD_NONE;
    }
    const char *after = NULL;
    const char *k = find_nocase(body, "VARSHA");
    sms_cmd_t cmd = SMS_CMD_NONE;
    if (k) {
        const char *p = k + 6;
        while (is_space(*p)) {
            p++;
        }
        cmd = match_word(p, &after);
    } else {
        const char *h = strchr(body, '#');
        if (h) {
            cmd = match_word(h + 1, &after);
        } else {
            /* The old 1768 accepted GET_GPS with no # or keyword */
            const char *g = find_nocase(body, "GET_GPS");
            if (g) {
                cmd = match_word(g, &after);
            }
        }
    }
    if (cmd != SMS_CMD_NONE && cmd != SMS_CMD_UNKNOWN && after) {
        take_arg(after, arg, arg_size);
    }
    return cmd;
}

bool sms_cmd_needs_office(sms_cmd_t cmd)
{
    switch (cmd) {
    case SMS_CMD_SET_TIME:
    case SMS_CMD_SET_STN_ID:
    case SMS_CMD_SET_SERVER_2:
    case SMS_CMD_SET_SERVER_MODE:
    case SMS_CMD_ADD_NUM:
    case SMS_CMD_DEL_NUM:
    case SMS_CMD_GET_NUM:   /* The list itself is only for the office */
    case SMS_CMD_STOP:
    case SMS_CMD_RESUME:
    case SMS_CMD_ERASE_DATA:
        return true;
    default:
        return false;
    }
}

bool sms_number_last10(const char *number, char out[11])
{
    char digits[24];
    size_t n = 0;
    for (const char *p = number; p && *p && n < sizeof(digits); p++) {
        if (*p >= '0' && *p <= '9') {
            digits[n++] = *p;
        }
    }
    if (n < 10) {
        out[0] = '\0';
        return false;
    }
    memcpy(out, digits + n - 10, 10);
    out[10] = '\0';
    return true;
}

bool sms_number_same(const char *a, const char *b)
{
    char x[11], y[11];
    return sms_number_last10(a, x) && sms_number_last10(b, y) && strcmp(x, y) == 0;
}

bool sms_parse_time(const char *arg, char out[15])
{
    size_t n = 0;
    for (const char *p = arg; *p && n < 14; p++) {
        if (*p >= '0' && *p <= '9') {
            out[n++] = *p;
        } else if (*p != '-' && *p != ':' && *p != ' ' && *p != 'T' && *p != '/' && *p != ',') {
            return false;
        }
    }
    out[n] = '\0';
    if (n != 14) {
        return false;
    }
    int yr = (out[0]-'0')*1000 + (out[1]-'0')*100 + (out[2]-'0')*10 + (out[3]-'0');
    int mo = (out[4]-'0')*10 + (out[5]-'0');
    int da = (out[6]-'0')*10 + (out[7]-'0');
    int hr = (out[8]-'0')*10 + (out[9]-'0');
    int mi = (out[10]-'0')*10 + (out[11]-'0');
    int se = (out[12]-'0')*10 + (out[13]-'0');
    return yr >= 2024 && yr <= 2079 && mo >= 1 && mo <= 12 && da >= 1 && da <= 31 && hr <= 23 && mi <= 59 && se <= 59;
}

bool sms_parse_station_id(const char *arg, int digits, char *out, size_t out_size)
{
    size_t n = 0;
    bool nonzero = false;
    for (const char *p = arg; *p; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        nonzero |= (*p != '0');
        n++;
    }
    if (n == 0 || (int)n > digits || !nonzero || out_size < (size_t)digits + 1) {
        return false;
    }
    size_t pad = (size_t)digits - n;
    memset(out, '0', pad);
    memcpy(out + pad, arg, n);
    out[digits] = '\0';
    return true;
}

/* value of "name=" up to ';' or end, into out */
static bool field(const char *arg, const char *name, char *out, size_t out_size)
{
    const char *p = find_nocase(arg, name);
    if (!p) {
        return false;
    }
    p += strlen(name);
    size_t n = 0;
    while (p[n] && p[n] != ';' && !is_space(p[n])) {
        if (n + 1 >= out_size) {
            return false;   /* Too long: refused rather than cut */
        }
        out[n] = p[n];
        n++;
    }
    out[n] = '\0';
    return true;
}

bool sms_parse_server2(const char *arg, char *host, size_t host_size, uint16_t *port,
                       char *path, size_t path_size, char *key, size_t key_size, bool *off)
{
    *off = false;
    char word[8];
    size_t n = 0;
    while (arg[n] && n + 1 < sizeof(word)) {
        word[n] = up(arg[n]);
        n++;
    }
    word[n] = '\0';
    if (strcmp(word, "OFF") == 0 || strcmp(word, "NONE") == 0) {
        *off = true;
        host[0] = path[0] = key[0] = '\0';
        *port = 0;
        return true;
    }
    char port_s[8] = "";
    if (!field(arg, "host=", host, host_size) || host[0] == '\0') {
        return false;
    }
    if (!field(arg, "path=", path, path_size) || path[0] != '/') {
        return false;
    }
    if (field(arg, "port=", port_s, sizeof(port_s)) && port_s[0]) {
        char *end = NULL;
        long v = strtol(port_s, &end, 10);
        if (*end != '\0' || v < 1 || v > 65535) {
            return false;
        }
        *port = (uint16_t)v;
    } else {
        *port = 80;
    }
    if (!field(arg, "key=", key, key_size)) {
        key[0] = '\0';
    }
    return true;
}

bool sms_payload_with_key(const char *payload, const char *key, char *out, size_t out_size)
{
    const char *start = NULL, *end = NULL;
    const char *f = strstr(payload, "&key=");
    if (f) {
        start = f + 5;
        end = strchr(start, '&');
        if (!end) {
            end = start + strlen(start);
        }
    } else {
        const char *j = strstr(payload, "\"ApiKey\":\"");
        if (j) {
            start = j + 10;
            end = strchr(start, '"');
        }
    }
    if (!start || !end) {
        return false;
    }
    size_t head = (size_t)(start - payload);
    size_t klen = strlen(key);
    size_t tail = strlen(end);
    if (head + klen + tail + 1 > out_size) {
        return false;
    }
    memcpy(out, payload, head);
    memcpy(out + head, key, klen);
    memcpy(out + head + klen, end, tail + 1);
    return true;
}

/* Days since 2000-03-01 style civil count (any fixed origin works: only differences are used) */
static long days_from_civil(int y, int m, int d)
{
    y -= (m <= 2);
    long era = y / 400;
    long yoe = y - era * 400;
    long doy = (153L * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe;
}

static bool two(const char *p, int *out)
{
    if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') {
        return false;
    }
    *out = (p[0] - '0') * 10 + (p[1] - '0');
    return true;
}

bool sms_age_minutes(const char *sms_ts, const char *now14, long *out_minutes)
{
    int yy, mo, dd, hh, mi, ss;
    /* "yy/MM/dd,hh:mm:ss" */
    if (!sms_ts || strlen(sms_ts) < 17 || sms_ts[2] != '/' || sms_ts[5] != '/' || sms_ts[8] != ',' ||
        !two(sms_ts, &yy) || !two(sms_ts + 3, &mo) || !two(sms_ts + 6, &dd) ||
        !two(sms_ts + 9, &hh) || !two(sms_ts + 12, &mi) || !two(sms_ts + 15, &ss)) {
        return false;
    }
    if (!now14 || strlen(now14) < 12) {
        return false;
    }
    int nc, ny, nmo, nd, nh, nmi;
    if (!two(now14, &nc) || !two(now14 + 2, &ny) || !two(now14 + 4, &nmo) || !two(now14 + 6, &nd) ||
        !two(now14 + 8, &nh) || !two(now14 + 10, &nmi) || nc != 20 || ny < 24) {
        return false;   /* No valid clock (lost, year 2000): the age is unknown, not huge */
    }
    if (mo < 1 || mo > 12 || dd < 1 || dd > 31 || nmo < 1 || nmo > 12 || nd < 1 || nd > 31) {
        return false;
    }
    long a = days_from_civil(2000 + yy, mo, dd) * 1440L + hh * 60L + mi;
    long b = days_from_civil(2000 + ny, nmo, nd) * 1440L + nh * 60L + nmi;
    *out_minutes = b - a;
    return true;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool sms_decode_ucs2(char *body)
{
    size_t n = 0;
    while (body[n] && body[n] != '\r' && body[n] != '\n') {
        if (hexval(body[n]) < 0) {
            return false;
        }
        n++;
    }
    if (n < 8 || (n % 4) != 0) {
        return false;
    }
    for (size_t i = 0; i < n; i += 4) {
        if (body[i] != '0' || body[i + 1] != '0' || hexval(body[i + 2]) > 7) {
            return false;   /* Not plain ASCII: left as it is */
        }
    }
    size_t o = 0;
    for (size_t i = 0; i < n; i += 4) {
        body[o++] = (char)(hexval(body[i + 2]) * 16 + hexval(body[i + 3]));
    }
    body[o] = '\0';
    return true;
}
