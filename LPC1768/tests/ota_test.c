/* Host test for ota.c: a pretend server, card and clock. Build and run from bench/host_tests.sh.
 * What it proves: when the unit asks at all, that a download ends up byte for byte on the card
 * through bad replies, a lost connection, the slot time limit and a changed or withdrawn file, that
 * an install is started only for a whole, checked file with time to spare, and that an image which
 * did not stay is never taken again. */
#include "ota.h"
#include "health.h"
#include "user_config.h"
#include "sim7600_driver.h"
#include "storage_log.h"
#include "flash_nvram.h"
#include "fw_update.h"
#include "fw_ctrl.h"
#include "boot/fw_journal.h"
#include "fw_app.h"
#include "hal_power.h"
#include "FreeRTOS.h"
#include "task.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_fails = 0, s_checks_run = 0;
#define CHECK(cond) do { s_checks_run++; if (!(cond)) { s_fails++; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

/* ---- clock ---- */
static uint32_t s_tick = 1000;
static int s_minute = 1, s_second = 10;     /* Time of day at tick s_tick_base */
static uint32_t s_tick_base = 1000;
portTickType xTaskGetTickCount(void) { return s_tick; }
void vTaskDelay(portTickType ticks) { s_tick += ticks; }
static void set_clock(int minute, int second) { s_minute = minute; s_second = second; s_tick_base = s_tick; }

/* ---- unit ---- */
static float s_battery = 12.6f;
static float s_rain_live = 0.0f;     /* v6.9.63: rain so far in this slot (CTRL install waits for none) */
static uint32_t s_running = 0x11111111UL;
static bool s_trial = false, s_sd_ok = true;
static uint32_t s_backlog = 0;
static const char *s_station = "001922";
bool intermcu_get_telemetry(telemetry_data_t *out)
{
    memset(out, 0, sizeof(*out));
    uint32_t sec = (uint32_t)s_minute * 60U + (uint32_t)s_second + (s_tick - s_tick_base) / 1000U;
    snprintf(out->present_time, sizeof(out->present_time), "2026111713%02u%02u", (unsigned)((sec / 60U) % 60U), (unsigned)(sec % 60U));
    out->bat1_volt = s_battery;
    out->rf_live_15m_mm = s_rain_live;
    return true;
}
const char *flash_nvram_get_station_id(void) { return s_station; }
uint32_t fw_app_running_crc(void) { return s_running; }
/* ---- v6.9.63: the CTRL ---- */
static uint32_t s_ctrl_crc = 0x33333333UL;
static bool s_ctrl_answers = true, s_ctrl_trial = false;
static fw_result_t s_ctrl_send_result = FW_OK;
static int s_ctrl_sent = 0, s_ctrl_restarts = 0, s_ctrl_checked = 0;
uint32_t fw_app_ctrl_crc(void) { return s_ctrl_crc; }
bool fw_ctrl_query(fw_ctrl_state_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = s_ctrl_trial ? FW_J_TRIAL_BOOT : FW_J_CONFIRMED; out->crc32 = s_ctrl_crc;
    return s_ctrl_answers;
}
fw_result_t fw_ctrl_send_image(const char *name, const fw_file_info_t *checked, fw_progress_fn progress)
{
    (void)name; (void)checked; progress("SEND", 10);
    s_ctrl_sent++; s_tick += 70000;
    return s_ctrl_send_result;
}
void fw_ctrl_restart_lpc845(void) { s_ctrl_restarts++; }
bool fw_app_trial_unconfirmed(void) { return s_trial; }
void wd_checkin(void) {}
void hal_power_heartbeat(void) {}
static jmp_buf s_reset;
static int s_resets = 0;
void NVIC_SystemReset(void) { s_resets++; longjmp(s_reset, 1); }

/* ---- card ---- */
#define FILES 6
static struct { char name[16]; uint8_t *data; uint32_t size; bool used; } s_files[FILES];
static int s_open = -1;
static bool s_card_write_fails = false;
static int file_find(const char *name)
{
    for (int i = 0; i < FILES; i++) if (s_files[i].used && !strcmp(s_files[i].name, name)) return i;
    return -1;
}
static int file_make(const char *name)
{
    int i = file_find(name);
    if (i >= 0) return i;
    for (i = 0; i < FILES; i++) if (!s_files[i].used) {
        s_files[i].used = true; s_files[i].size = 0; s_files[i].data = calloc(1, 700000);
        snprintf(s_files[i].name, sizeof(s_files[i].name), "%s", name);
        return i;
    }
    abort();
}
static void file_set(const char *name, const void *data, uint32_t size)
{
    int i = file_make(name); memcpy(s_files[i].data, data, size); s_files[i].size = size;
}
bool storage_sd_is_ok(void) { return s_sd_ok; }
uint32_t storage_backlog_get_pending_count(void) { return s_backlog; }
void storage_log_flush(void) {}
static char s_log[8192];
void storage_log_debug(const char *tag, const char *msg)
{
    size_t n = strlen(s_log);
    snprintf(s_log + n, sizeof(s_log) - n, "[%s] %s\n", tag, msg);
}
bool storage_state_load(const char *name, char *out, size_t size)
{
    int i = file_find(name);
    if (i < 0 || s_files[i].size == 0) return false;
    uint32_t n = s_files[i].size < size - 1 ? s_files[i].size : (uint32_t)size - 1;
    memcpy(out, s_files[i].data, n); out[n] = 0;
    return true;
}
bool storage_state_save(const char *name, const char *text)
{
    int i = file_make(name);    /* Overwrites in place, as the firmware's does */
    memcpy(s_files[i].data, text, strlen(text));
    if (s_files[i].size < strlen(text)) s_files[i].size = (uint32_t)strlen(text);
    return true;
}
void storage_state_delete(const char *name)
{
    int i = file_find(name);
    if (i >= 0) { CHECK(i != s_open); free(s_files[i].data); s_files[i].used = false; }
}
long storage_file_size(const char *name)
{
    CHECK(s_open < 0);      /* Shares the firmware file object */
    int i = file_find(name);
    return i < 0 ? -1 : (long)s_files[i].size;
}
bool storage_ota_open(const char *name, uint32_t expect)
{
    CHECK(s_open < 0);
    int i = file_find(name);
    if (expect == 0) { i = file_make(name); s_files[i].size = 0; }
    if (i < 0 || s_files[i].size != expect) return false;
    s_open = i;
    return true;
}
bool storage_ota_write(const void *data, uint32_t len)
{
    CHECK(s_open >= 0);
    if (s_card_write_fails) return false;
    memcpy(s_files[s_open].data + s_files[s_open].size, data, len);
    s_files[s_open].size += len;
    return true;
}
void storage_ota_close(void) { CHECK(s_open >= 0); s_open = -1; }

/* ---- the update code ---- */
static uint8_t s_fw[400000];
static uint32_t s_fw_size, s_fw_file_crc, s_fw_image_crc;
static int s_checked = 0, s_staged = 0;
static fw_result_t s_check_result = FW_OK;
static uint32_t s_check_crc_xor = 0;     /* The file holds another image than the one offered */
static uint32_t crc32z(const uint8_t *d, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFUL;
    while (n--) { c ^= *d++; for (int b = 0; b < 8; b++) c = (c >> 1) ^ (0xEDB88320UL & (0UL - (c & 1UL))); }
    return ~c;
}
fw_result_t fw_update_check(const char *name, uint32_t profile, fw_file_info_t *info, fw_progress_fn progress)
{
    (void)profile;
    s_checked++;
    CHECK(s_open < 0);
    progress("CHECK", 50);
    int i = file_find(name);
    if (i < 0) return FW_E_NOT_FOUND;
    if (s_check_result != FW_OK) return s_check_result;
    if (s_files[i].size != s_fw_size || memcmp(s_files[i].data, s_fw, s_fw_size)) return FW_E_BAD_LINE;
    memset(info, 0, sizeof(*info));
    info->crc32 = s_fw_image_crc ^ s_check_crc_xor; info->length = 1; snprintf(info->version, sizeof(info->version), "v9.9.9");
    return FW_OK;
}
fw_result_t fw_update_stage(const char *name, const fw_file_info_t *checked, fw_progress_fn progress)
{
    (void)name; (void)checked; (void)progress;
    s_staged++;
    return FW_OK;
}
const char *fw_update_error_text(fw_result_t r) { (void)r; return "TEST ERROR"; }
fw_result_t fw_ctrl_check(const char *name, fw_file_info_t *info, fw_progress_fn progress)
{
    s_ctrl_checked++;
    return fw_update_check(name, 0, info, progress);    /* Same pretend file check */
}

/* ---- server and modem ---- */
static enum { SRV_NONE, SRV_OFFER, SRV_CTRL } s_server = SRV_NONE;
static bool s_server_dead = false;
static int s_get_calls = 0, s_check_calls = 0, s_piece_calls = 0, s_closes = 0;
static char s_last_check[512];
static uint32_t s_lowest_offset;
static int s_fault_every = 0;       /* Every Nth piece request goes wrong, in turn: no reply, bad byte, short */
static int s_fault_kind = 0;
static bool s_all_pieces_bad = false, s_gone = false;
static uint32_t s_piece_ms = 500;
static char s_reply[2100];
const char *sim7600_http_get(const char *host, uint16_t port, const char *path, int *len, int *status)
{
    CHECK(!strcmp(host, s_files[0].used && file_find("OTASRV.DAT") >= 0 ? "10.1.2.3" : OTA_SERVER_HOST));
    CHECK(port == (file_find("OTASRV.DAT") >= 0 ? 8080 : 80));
    CHECK(s_open < 0 || strncmp(path, "/lpc/check", 10));
    CHECK(strlen(path) < 440);
    s_get_calls++;
    *len = 0; *status = 0;
    if (s_server_dead) { s_tick += 15000; return NULL; }
    if (!strncmp(path, "/lpc/check?", 11)) {
        s_check_calls++;
        snprintf(s_last_check, sizeof(s_last_check), "%s", path);
        s_tick += 1500;
        if (s_server == SRV_OFFER) snprintf(s_reply, sizeof(s_reply), "OTA FW %lu %08lX %08lX\n", (unsigned long)s_fw_size, (unsigned long)s_fw_file_crc, (unsigned long)s_fw_image_crc);
        else if (s_server == SRV_CTRL) snprintf(s_reply, sizeof(s_reply), "OTA CTRL %lu %08lX %08lX\n", (unsigned long)s_fw_size, (unsigned long)s_fw_file_crc, (unsigned long)s_fw_image_crc);
        else snprintf(s_reply, sizeof(s_reply), "OTA NONE\n");
        *status = 200; *len = (int)strlen(s_reply);
        return s_reply;
    }
    unsigned long crc = 0, off = 0, n = 0;
    char id[16];
    CHECK(sscanf(path, "/lpc/fw?id=%15[^&]&c=%8lx&o=%lu&n=%lu", id, &crc, &off, &n) == 4);
    CHECK(!strcmp(id, s_station) && n >= 1 && n <= 1024);
    s_piece_calls++;
    s_tick += s_piece_ms;
    if (off < s_lowest_offset) s_lowest_offset = (uint32_t)off;
    if (s_gone || crc != s_fw_file_crc) { snprintf(s_reply, sizeof(s_reply), "OTA GONE\n"); *status = 404; *len = 9; return s_reply; }
    if (off >= s_fw_size) { snprintf(s_reply, sizeof(s_reply), "OTA END\n"); *status = 416; *len = 8; return s_reply; }
    if (off + n > s_fw_size) n = s_fw_size - off;
    snprintf(s_reply, sizeof(s_reply), "@%08lX\n", (unsigned long)crc32z(s_fw + off, (uint32_t)n));
    memcpy(s_reply + 10, s_fw + off, n); s_reply[10 + n] = 0;
    *status = 200; *len = (int)n + 10;
    if (s_all_pieces_bad || (s_fault_every && (s_piece_calls % s_fault_every) == 0)) {
        switch (s_all_pieces_bad ? 1 : (s_fault_kind++ % 4)) {
        case 0: *len = 0; *status = 0; return NULL;                 /* No reply */
        case 1: s_reply[10 + n / 2] ^= 1; break;                    /* A changed byte */
        case 2: *len -= 3; break;                                   /* Short */
        default: s_reply[0] = '#'; break;                           /* Not a piece */
        }
    }
    return s_reply;
}
void sim7600_http_get_close(void) { s_closes++; }
/* ---- health report ---- */
static int s_health_built = 0, s_health_sent = 0;
static uint32_t s_health_pending = 99;
const char *health_query(const telemetry_data_t *t, uint32_t pending)
{
    (void)t; s_health_built++; s_health_pending = pending;
    return "&hs=OK&bv=12.60&ok=95";
}
void health_sent(void) { s_health_sent++; }
const char *sim7600_http_get_fail_text(void) { return s_server_dead ? "CIPOPEN: +CIPOPEN: 0,4 ERROR" : ""; }

/* ---- helpers ---- */
static const int DAILY = OTA_DAILY_FIRST_SLOT + 1922 % OTA_DAILY_SPREAD_SLOTS;
static bool step(int slot)      /* true if the unit restarted to install */
{
    s_log[0] = 0;
    const int s_resets_before = s_resets, s_checked_before = s_checked;
    if (setjmp(s_reset)) { s_open = -1; return true; }
    ota_slot_step(slot);
    CHECK(s_open < 0);
    CHECK(s_resets == s_resets_before && s_checked == s_checked_before);   /* Never from inside the slot */
    s_tick += 2000;         /* The slot's bookkeeping */
    ota_install_step();
    return false;
}
static void fresh(void)
{
    for (int i = 0; i < FILES; i++) if (s_files[i].used) { free(s_files[i].data); s_files[i].used = false; }
    s_open = -1; s_tick += 5000; set_clock(1, 10);
    s_battery = 12.6f; s_running = 0x11111111UL; s_trial = false; s_sd_ok = true; s_backlog = 0; s_card_write_fails = false;
    s_server = SRV_NONE; s_server_dead = false; s_get_calls = s_check_calls = s_piece_calls = s_closes = 0;
    s_fault_every = 0; s_fault_kind = 0; s_all_pieces_bad = false; s_gone = false; s_piece_ms = 500;
    s_health_built = s_health_sent = 0; s_health_pending = 99;
    s_checked = s_staged = s_resets = 0; s_check_result = FW_OK; s_lowest_offset = 0xFFFFFFFFUL; s_last_check[0] = 0;
    s_ctrl_crc = 0x33333333UL; s_ctrl_answers = true; s_ctrl_trial = false; s_ctrl_send_result = FW_OK;
    s_ctrl_sent = s_ctrl_restarts = s_ctrl_checked = 0; s_rain_live = 0.0f;
}
static void new_firmware(uint32_t size, unsigned seed)
{
    srand(seed);
    for (uint32_t i = 0; i < size; i++) s_fw[i] = (uint8_t)"0123456789ABCDEF:\r\n"[rand() % 19];
    s_fw_size = size; s_fw_file_crc = crc32z(s_fw, size); s_fw_image_crc = 0x22220000UL + seed;
}
static bool card_has_whole_file(void)
{
    int i = file_find(OTA_FILE_FILENAME);
    return i >= 0 && s_files[i].size == s_fw_size && !memcmp(s_files[i].data, s_fw, s_fw_size);
}
static char phase(void)
{
    char text[64];
    return (storage_state_load(OTA_STATE_FILENAME, text, sizeof(text)) && strlen(text) >= 27) ? text[26] : '?';
}

int main(void)
{
    new_firmware(340936, 1);

    /* 1. When it asks at all */
    fresh(); CHECK(!step(DAILY + 1)); CHECK(s_get_calls == 0);
    fresh(); CHECK(!step(DAILY)); CHECK(s_check_calls == 1 && s_piece_calls == 0 && s_closes >= 1);
    CHECK(strstr(s_last_check, "id=001922&p=") && strstr(s_last_check, "&v=" FIRMWARE_VERSION "&c=11111111&s=0&t=00000000"));
    CHECK(file_find(OTA_FILE_FILENAME) < 0);
    fresh(); file_set(OTA_NOW_FILENAME, "", 0); CHECK(!step(3)); CHECK(s_check_calls == 1);
    fresh(); s_battery = 10.8f; CHECK(!step(DAILY + 1)); CHECK(s_get_calls == 0);  /* Ordinary slot, low battery: nothing */
    fresh(); s_battery = 0.2f; CHECK(!step(DAILY)); CHECK(s_check_calls == 1);     /* Broken sense line: allowed */
    fresh(); s_backlog = 2; CHECK(!step(DAILY)); CHECK(s_check_calls == 1 && s_piece_calls == 0);   /* v6.9.60: report goes */
    fresh(); s_trial = true; CHECK(!step(DAILY)); CHECK(s_get_calls == 0);
    fresh(); s_sd_ok = false; CHECK(!step(DAILY)); CHECK(s_get_calls == 0);
    fresh(); s_server_dead = true; CHECK(!step(DAILY)); CHECK(s_check_calls == 0 && s_get_calls == 1 && phase() == '?');
    fresh(); file_set(OTA_SERVER_FILENAME, "10.1.2.3 8080\r\n", 15); CHECK(!step(DAILY)); CHECK(s_check_calls == 1);

    /* 2. A clean download and install, then the report */
    fresh(); s_server = SRV_OFFER;
    CHECK(step(DAILY));
    CHECK(s_resets == 1 && s_checked == 1 && s_staged == 1 && card_has_whole_file() && phase() == 'I');
    CHECK(s_piece_calls == (int)((s_fw_size + 1023) / 1024));
    s_running = s_fw_image_crc; s_tick += 900000; set_clock(16, 10);
    s_server = SRV_NONE;
    CHECK(!step(DAILY + 1));    /* Not its daily slot: it still reports */
    CHECK(s_check_calls == 2 && strstr(s_last_check, "&c=22220001&s=0&t=00000000"));
    CHECK(phase() == '-' && file_find(OTA_FILE_FILENAME) < 0 && strstr(s_log, "new firmware is running"));
    CHECK(!step(DAILY + 2)); CHECK(s_check_calls == 2);

    /* 3. The new image did not stay: said once to the server, never taken again; a newer one is */
    fresh(); s_server = SRV_OFFER;
    CHECK(step(DAILY));
    s_tick += 900000; set_clock(16, 10);
    CHECK(!step(DAILY + 1));
    CHECK(phase() == 'F' && strstr(s_last_check, "&c=11111111&s=3&t=22220001") && file_find(OTA_FILE_FILENAME) < 0);
    CHECK(s_staged == 1 && strstr(s_log, "failed here before"));
    int pieces = s_piece_calls;
    s_tick += 900000; set_clock(1, 10);
    CHECK(!step(DAILY)); CHECK(s_piece_calls == pieces && s_staged == 1 && phase() == 'F');
    new_firmware(200000, 2);
    s_tick += 900000; set_clock(1, 10);
    CHECK(step(DAILY)); CHECK(s_staged == 2 && card_has_whole_file() && phase() == 'I');
    new_firmware(340936, 1);

    /* 4. Bad replies of every kind in between: the file is still exact, nothing is asked twice in vain */
    fresh(); s_server = SRV_OFFER; s_fault_every = 7;
    CHECK(step(DAILY)); CHECK(card_has_whole_file() && s_staged == 1);

    /* 5. Nothing but bad replies: stops after 3, keeps what it has, goes on at the next slot */
    fresh(); s_server = SRV_OFFER; s_all_pieces_bad = true;
    CHECK(!step(DAILY)); CHECK(s_piece_calls == 3 && s_staged == 0 && phase() == 'D');
    s_all_pieces_bad = false; s_tick += 900000; set_clock(1, 10);
    CHECK(step(DAILY + 1)); CHECK(card_has_whole_file());

    /* 6. A slow link: stops in time for the next slot, and goes on from where it was */
    fresh(); s_server = SRV_OFFER; s_piece_ms = 4000;
    CHECK(!step(DAILY));
    int at = file_find(OTA_FILE_FILENAME);
    CHECK(at >= 0 && s_files[at].size > 0 && s_files[at].size < s_fw_size && s_files[at].size % 1024 == 0 && phase() == 'D');
    CHECK(strstr(s_log, "next slot is near") && s_staged == 0);
    {
        telemetry_data_t now; intermcu_get_telemetry(&now);
        int min = (now.present_time[10] - '0') * 10 + (now.present_time[11] - '0');
        int sec = (now.present_time[12] - '0') * 10 + (now.present_time[13] - '0');
        CHECK((15 - min % 15) * 60 - sec >= OTA_STOP_BEFORE_SLOT_S_FOR_TEST - 4 && (15 - min % 15) * 60 - sec <= OTA_STOP_BEFORE_SLOT_S_FOR_TEST);
    }
    uint32_t had = s_files[at].size;
    s_tick += 400000; set_clock(16, 10); s_lowest_offset = 0xFFFFFFFFUL; s_piece_ms = 300;
    CHECK(step(DAILY + 1)); CHECK(s_lowest_offset == had && card_has_whole_file());

    /* 7. File whole but the slot is near: installed after the next slot, with nothing fetched again */
    fresh(); s_server = SRV_OFFER; file_set(OTA_FILE_FILENAME, s_fw, s_fw_size);
    {
        char text[40]; snprintf(text, sizeof(text), "%08lX %08lX %07lu D\r\n", (unsigned long)s_fw_file_crc, (unsigned long)s_fw_image_crc, (unsigned long)s_fw_size);
        file_set(OTA_STATE_FILENAME, text, (uint32_t)strlen(text));
    }
    set_clock(10, 30);
    CHECK(!step(DAILY)); CHECK(s_piece_calls == 0 && s_checked == 0 && s_staged == 0 && card_has_whole_file());
    s_tick += 300000; set_clock(16, 10);
    CHECK(step(DAILY + 1)); CHECK(s_piece_calls == 0 && s_staged == 1);

    /* 8. The office withdraws the file mid-download, or replaces it */
    fresh(); s_server = SRV_OFFER; s_piece_ms = 4000;
    CHECK(!step(DAILY)); CHECK(phase() == 'D');
    s_gone = true; s_tick += 400000; set_clock(16, 10); s_piece_ms = 300;
    CHECK(!step(DAILY + 1)); CHECK(phase() == '-' && file_find(OTA_FILE_FILENAME) < 0 && s_staged == 0);
    fresh(); s_server = SRV_OFFER; s_piece_ms = 4000;
    CHECK(!step(DAILY));
    new_firmware(300000, 3); s_tick += 400000; set_clock(16, 10); s_piece_ms = 300; s_lowest_offset = 0xFFFFFFFFUL;
    CHECK(step(DAILY + 1)); CHECK(s_lowest_offset == 0 && card_has_whole_file());
    new_firmware(340936, 1);
    /* ... or stops offering it */
    fresh(); s_server = SRV_OFFER; s_piece_ms = 4000;
    CHECK(!step(DAILY)); s_server = SRV_NONE; s_tick += 400000; set_clock(16, 10);
    CHECK(!step(DAILY + 1)); CHECK(phase() == '-' && file_find(OTA_FILE_FILENAME) < 0);

    /* 9. A file the update check refuses is dropped and not fetched again */
    fresh(); s_server = SRV_OFFER; s_check_result = FW_E_WRONG_PROFILE;
    CHECK(!step(DAILY)); CHECK(s_staged == 0 && phase() == 'F' && file_find(OTA_FILE_FILENAME) < 0);
    pieces = s_piece_calls; s_tick += 900000; set_clock(1, 10);
    CHECK(!step(DAILY)); CHECK(s_piece_calls == pieces);
    /* The image inside is not the one offered */
    fresh(); s_server = SRV_OFFER; s_check_crc_xor = 0x10;
    CHECK(!step(DAILY)); CHECK(s_checked == 1 && s_staged == 0 && phase() == 'F' && file_find(OTA_FILE_FILENAME) < 0);
    s_check_crc_xor = 0;

    /* 10. Card trouble */
    new_firmware(340936, 1);
    fresh(); s_server = SRV_OFFER; s_card_write_fails = true;
    CHECK(!step(DAILY)); CHECK(s_staged == 0 && phase() == 'D' && strstr(s_log, "card write failed"));
    /* A file on the card longer than the one offered (left by something else): started afresh */
    fresh(); s_server = SRV_OFFER; file_set(OTA_FILE_FILENAME, s_fw, s_fw_size); s_files[file_find(OTA_FILE_FILENAME)].size += 5000;
    {
        char text[40]; snprintf(text, sizeof(text), "%08lX %08lX %07lu D\r\n", (unsigned long)s_fw_file_crc, (unsigned long)s_fw_image_crc, (unsigned long)s_fw_size);
        file_set(OTA_STATE_FILENAME, text, (uint32_t)strlen(text));
    }
    s_lowest_offset = 0xFFFFFFFFUL;
    CHECK(step(DAILY)); CHECK(s_lowest_offset == 0 && card_has_whole_file());
    /* OTA.DAT damaged: treated as no download */
    fresh(); s_server = SRV_NONE; file_set(OTA_STATE_FILENAME, "garbage here, not a state\r\n", 27);
    CHECK(!step(DAILY)); CHECK(s_check_calls == 1 && strstr(s_last_check, "&s=0&t=00000000"));

    /* 11. An offer of what is already running, or of a silly size, is ignored */
    fresh(); s_server = SRV_OFFER; s_running = s_fw_image_crc;
    CHECK(!step(DAILY)); CHECK(s_piece_calls == 0 && phase() == '?');
    fresh(); s_server = SRV_OFFER; { uint32_t keep = s_fw_size; s_fw_size = 700000; CHECK(!step(DAILY)); CHECK(s_piece_calls == 0); s_fw_size = keep; }

    /* 12. v6.9.57: a failed daily check is tried at the next 3 slots that day, then not; the reason is logged */
    fresh(); s_server_dead = true;
    CHECK(!step(DAILY)); CHECK(s_get_calls == 1 && file_find(OTA_RETRY_FILENAME) >= 0);
    CHECK(strstr(s_log, "no answer from") && strstr(s_log, "CIPOPEN: +CIPOPEN: 0,4 ERROR") && strstr(s_log, "ask again"));
    s_tick += 900000; set_clock(16, 10);
    CHECK(!step(DAILY + 1)); CHECK(s_get_calls == 2);
    s_tick += 900000; set_clock(31, 10);
    CHECK(!step(DAILY + 2)); CHECK(s_get_calls == 3);
    s_tick += 900000; set_clock(46, 10);
    CHECK(!step(DAILY + 3)); CHECK(s_get_calls == 4 && file_find(OTA_RETRY_FILENAME) < 0);
    s_tick += 900000; set_clock(1, 10);
    CHECK(!step(DAILY + 4)); CHECK(s_get_calls == 4);
    /* ... and an answer on a retry ends the tries, and takes the offer */
    fresh(); s_server_dead = true;
    CHECK(!step(DAILY)); s_server_dead = false; s_server = SRV_OFFER; s_tick += 900000; set_clock(16, 10);
    CHECK(step(DAILY + 1)); CHECK(s_check_calls == 1 && s_staged == 1 && file_find(OTA_RETRY_FILENAME) < 0);
    /* ... a try left from another day is not taken */
    fresh(); file_set(OTA_RETRY_FILENAME, "20261116 1\r\n", 12);
    CHECK(!step(DAILY + 1)); CHECK(s_get_calls == 0);
    /* ... a check that is not due (OTA.NOW) leaves no tries behind */
    fresh(); s_server_dead = true; file_set(OTA_NOW_FILENAME, "", 0);
    CHECK(!step(DAILY + 1)); CHECK(s_get_calls == 1 && file_find(OTA_RETRY_FILENAME) < 0);
    /* Records still to send: a check that isn't due (OTA.NOW) is put off and says so; the twice-daily
     * check goes (health report), but a download waits for them (v6.9.60) */
    fresh(); s_backlog = 2;     /* A download under way, on an ordinary slot */
    {
        char text[40]; snprintf(text, sizeof(text), "%08lX %08lX %07lu D\r\n", (unsigned long)s_fw_file_crc, (unsigned long)s_fw_image_crc, (unsigned long)s_fw_size);
        file_set(OTA_STATE_FILENAME, text, (uint32_t)strlen(text));
    }
    CHECK(!step(DAILY + 1)); CHECK(s_get_calls == 0 && strstr(s_log, "2 record(s) still to send"));
    fresh(); s_backlog = 2; file_set(OTA_NOW_FILENAME, "", 0); s_server = SRV_OFFER;    /* Bench: checks, reports, no download */
    CHECK(!step(DAILY + 1)); CHECK(s_check_calls == 1 && s_piece_calls == 0 && s_health_built == 1 && strstr(s_log, "download put off"));
    fresh(); s_backlog = 0; CHECK(!step(DAILY + 1)); CHECK(!strstr(s_log, "still to send"));   /* Not due: silent */
    fresh(); s_backlog = 3; s_server = SRV_OFFER;
    CHECK(!step(DAILY)); CHECK(s_check_calls == 1 && s_piece_calls == 0 && s_health_pending == 3 && s_health_sent == 1);
    CHECK(strstr(s_log, "download put off: 3 record(s)") && phase() == '?');
    s_backlog = 0; s_tick += 900000; set_clock(1, 10);
    CHECK(step(OTA_NIGHT_FIRST_SLOT + 1922 % OTA_DAILY_SPREAD_SLOTS)); CHECK(s_staged == 1);   /* Queue empty: taken */
    /* The install report goes with records waiting, too */
    fresh(); s_server = SRV_OFFER; CHECK(step(DAILY)); s_running = s_fw_image_crc; s_server = SRV_NONE;
    s_backlog = 1; s_tick += 900000; set_clock(16, 10);
    CHECK(!step(DAILY + 1)); CHECK(phase() == '-' && s_check_calls == 2 && strstr(s_last_check, "&c=22220001&s=0"));

    /* 13. v6.9.60 health report: at the night and day check slots (and their retries), not on others */
    const int NIGHT = OTA_NIGHT_FIRST_SLOT + 1922 % OTA_DAILY_SPREAD_SLOTS;
    fresh(); CHECK(!step(NIGHT)); CHECK(s_check_calls == 1 && s_health_built == 1 && s_health_sent == 1);
    CHECK(strstr(s_last_check, "&t=00000000&hs=OK&bv=12.60&ok=95") && strstr(s_log, "health report sent"));
    fresh(); CHECK(!step(DAILY)); CHECK(s_health_built == 1 && s_health_sent == 1);
    fresh(); CHECK(!step(NIGHT + 1)); CHECK(s_get_calls == 0 && s_health_built == 0);
    fresh(); file_set(OTA_NOW_FILENAME, "", 0); CHECK(!step(DAILY + 5)); CHECK(s_check_calls == 1 && s_health_built == 1);
    /* Not answered: not counted as sent, and the retry carries the report again */
    fresh(); s_server_dead = true; CHECK(!step(NIGHT)); CHECK(s_health_built == 1 && s_health_sent == 0);
    s_server_dead = false; s_tick += 900000; set_clock(16, 10);
    CHECK(!step(NIGHT + 1)); CHECK(s_health_built == 2 && s_health_sent == 1);
    /* The install report alone carries no health report */
    fresh(); s_server = SRV_OFFER; CHECK(step(DAILY)); s_running = s_fw_image_crc; s_server = SRV_NONE;
    s_tick += 900000; set_clock(16, 10); s_health_built = 0;
    CHECK(!step(DAILY + 1)); CHECK(s_check_calls == 2 && s_health_built == 0);
    /* 14. v6.9.61: a low battery holds back only the download; the check and health report still go */
    fresh(); s_battery = 10.8f; s_server = SRV_OFFER;
    CHECK(!step(DAILY)); CHECK(s_check_calls == 1 && s_piece_calls == 0 && s_health_sent == 1 && phase() == '?');
    CHECK(strstr(s_log, "download put off: battery low") && strstr(s_log, "health report sent"));
    s_battery = 12.4f; s_tick += 900000; set_clock(1, 10);
    CHECK(step(NIGHT)); CHECK(s_staged == 1);                    /* Good battery at the next check: taken */
    /* ... a download under way stops on a low battery at an ordinary slot, goes on at the next good one */
    fresh(); s_server = SRV_OFFER; s_piece_ms = 60000; CHECK(!step(DAILY)); CHECK(phase() == 'D' && s_piece_calls > 0);
    s_piece_ms = 500; s_battery = 10.8f; s_tick += 900000; set_clock(16, 10);
    { int before = s_get_calls; CHECK(!step(DAILY + 1)); CHECK(s_get_calls == before && strstr(s_log, "battery low")); }
    s_battery = 12.4f; s_tick += 900000; set_clock(31, 10);
    CHECK(step(DAILY + 2)); CHECK(card_has_whole_file());
    /* ... the install report goes on a low battery */
    fresh(); s_server = SRV_OFFER; CHECK(step(DAILY)); s_running = s_fw_image_crc; s_server = SRV_NONE;
    s_battery = 10.8f; s_tick += 900000; set_clock(16, 10);
    CHECK(!step(DAILY + 1)); CHECK(phase() == '-' && s_check_calls == 2);

    /* ... 11.0 V is the limit (v6.9.61; was 11.8 V): 11.5 V downloads */
    fresh(); s_battery = 11.5f; s_server = SRV_OFFER; CHECK(step(DAILY)); CHECK(s_staged == 1);
    fresh(); s_battery = 11.0f; s_server = SRV_OFFER; CHECK(step(DAILY)); CHECK(s_staged == 1);
    fresh(); s_battery = 10.99f; s_server = SRV_OFFER; CHECK(!step(DAILY)); CHECK(s_piece_calls == 0 && s_health_sent == 1);

    /* 15. v6.9.61: the check slot's record not delivered: the next delivered slot that day asks, with the report */
    fresh(); ota_slot_not_sent(NIGHT, "20261117013102");
    CHECK(file_find(OTA_RETRY_FILENAME) >= 0 && strstr(s_log, "check slot not sent"));
    CHECK(!step(NIGHT + 1)); CHECK(s_check_calls == 1 && s_health_sent == 1 && file_find(OTA_RETRY_FILENAME) < 0);
    fresh(); ota_slot_not_sent(NIGHT + 1, "20261117014602"); CHECK(file_find(OTA_RETRY_FILENAME) < 0);   /* Not its check slot */
    fresh(); file_set(OTA_RETRY_FILENAME, "20261117 2\r\n", 12); ota_slot_not_sent(DAILY, "20261117133102");
    { char t[24]; CHECK(storage_state_load(OTA_RETRY_FILENAME, t, sizeof(t)) && !strncmp(t, "20261117 2", 10)); }   /* Tries kept */
    fresh(); s_sd_ok = false; ota_slot_not_sent(DAILY, "20261117133102"); CHECK(file_find(OTA_RETRY_FILENAME) < 0);
    s_sd_ok = true;
    /* ... unanswered after that: the usual 3 tries */
    fresh(); ota_slot_not_sent(DAILY, "20261117133102"); s_server_dead = true;
    CHECK(!step(DAILY + 1)); CHECK(s_get_calls == 1 && file_find(OTA_RETRY_FILENAME) >= 0);
    s_tick += 900000; set_clock(16, 10); CHECK(!step(DAILY + 2));
    s_tick += 900000; set_clock(31, 10); CHECK(!step(DAILY + 3)); CHECK(s_get_calls == 3 && file_find(OTA_RETRY_FILENAME) < 0);

    /* 16. v6.9.63: CTRL by OTA. The check says what the CTRL runs (cc), between p and v */
    fresh(); CHECK(!step(DAILY)); CHECK(strstr(s_last_check, "&cc=33333333&v=") && strstr(s_last_check, "id=001922&p="));
    /* A clean CTRL download and install: file to the card, sent to the CTRL, CTRL restarted, phase i */
    fresh(); s_server = SRV_CTRL;
    CHECK(step(DAILY));
    CHECK(card_has_whole_file() && s_ctrl_checked == 1 && s_ctrl_sent == 1 && s_ctrl_restarts == 1 && s_staged == 0 && phase() == 'i');
    CHECK(strstr(s_last_check, "&s=0&t=00000000"));
    /* Next slot: the CTRL runs it, said once, then quiet */
    s_ctrl_crc = s_fw_image_crc; s_server = SRV_NONE; s_tick += 900000; set_clock(16, 10);
    CHECK(!step(DAILY + 1)); CHECK(s_check_calls == 2 && strstr(s_last_check, "&cc=22220001&") && phase() == '-');
    CHECK(strstr(s_log, "new CTRL firmware is running") && file_find(OTA_FILE_FILENAME) < 0);
    CHECK(!step(DAILY + 2)); CHECK(s_check_calls == 2);
    /* The CTRL went back to its old firmware: said with s=3, never taken again */
    fresh(); s_server = SRV_CTRL; CHECK(step(DAILY));
    s_tick += 900000; set_clock(16, 10);
    CHECK(!step(DAILY + 1)); CHECK(phase() == 'f' && strstr(s_last_check, "&s=3&t=22220001") && strstr(s_log, "failed here before"));
    CHECK(s_ctrl_sent == 1);
    /* CTRL's running image not known yet after the restart: the report waits for it */
    fresh(); s_server = SRV_CTRL; CHECK(step(DAILY)); s_ctrl_crc = 0; s_server = SRV_NONE;
    s_tick += 900000; set_clock(16, 10);
    CHECK(!step(DAILY + 1)); CHECK(phase() == 'i');
    s_ctrl_crc = s_fw_image_crc; s_tick += 900000; set_clock(31, 10);
    CHECK(!step(DAILY + 2)); CHECK(phase() == '-');
    /* Raining: the whole file waits on the card, installed at the next dry slot without fetching again */
    fresh(); s_server = SRV_CTRL; s_rain_live = 0.5f;
    CHECK(!step(DAILY)); CHECK(card_has_whole_file() && s_ctrl_sent == 0 && phase() == 'd' && strstr(s_log, "raining"));
    pieces = s_piece_calls; s_rain_live = 0.0f; s_tick += 900000; set_clock(16, 10);
    CHECK(step(DAILY + 1)); CHECK(s_ctrl_sent == 1 && s_piece_calls == pieces && phase() == 'i');
    /* CTRL on trial: put off. CTRL without a bootloader: not taken */
    fresh(); s_server = SRV_CTRL; s_ctrl_trial = true;
    CHECK(!step(DAILY)); CHECK(s_ctrl_sent == 0 && phase() == 'd');
    fresh(); s_server = SRV_CTRL; s_ctrl_answers = false;
    CHECK(!step(DAILY)); CHECK(s_ctrl_sent == 0 && phase() == 'f' && file_find(OTA_FILE_FILENAME) < 0);
    /* The link failed while sending: fetched again at a later check, not marked failed */
    fresh(); s_server = SRV_CTRL; s_ctrl_send_result = FW_E_LINK;
    CHECK(!step(DAILY)); CHECK(s_ctrl_restarts == 0 && phase() == '-' && file_find(OTA_FILE_FILENAME) < 0);
    /* The CTRL refused the file: not taken again */
    fresh(); s_server = SRV_CTRL; s_ctrl_send_result = FW_E_BAD_CRC;
    CHECK(!step(DAILY)); CHECK(s_ctrl_restarts == 0 && phase() == 'f');
    /* Offer of what the CTRL already runs, or with the CTRL's image not known: ignored */
    fresh(); s_server = SRV_CTRL; s_ctrl_crc = s_fw_image_crc;
    CHECK(!step(DAILY)); CHECK(s_piece_calls == 0 && phase() == '?');
    fresh(); s_server = SRV_CTRL; s_ctrl_crc = 0;
    CHECK(!step(DAILY)); CHECK(s_piece_calls == 0 && strstr(s_last_check, "&cc=00000000&"));
    /* A main-board download under way, then a CTRL offer: the main file is dropped, the CTRL one fetched */
    fresh(); s_server = SRV_OFFER; s_piece_ms = 4000;
    CHECK(!step(DAILY)); CHECK(phase() == 'D');
    s_server = SRV_CTRL; s_piece_ms = 300; s_tick += 400000; set_clock(16, 10); s_lowest_offset = 0xFFFFFFFFUL;
    new_firmware(60000, 4);
    CHECK(step(DAILY + 1)); CHECK(s_lowest_offset == 0 && card_has_whole_file() && phase() == 'i' && s_staged == 0);
    new_firmware(340936, 1);

    /* No SD card on the daily slot: said */
    fresh(); s_sd_ok = false; CHECK(!step(DAILY)); CHECK(strstr(s_log, "SD card is not working"));

    printf("%s: ota_test, %d checks, %d failed\n", s_fails ? "FAILED" : "ALL PASS", s_checks_run, s_fails);
    return s_fails ? 1 : 0;
}
