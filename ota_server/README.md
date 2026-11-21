# LPC-AIO firmware-over-the-air server

One Python file, standard library only (`lpc_ota.py`), with one SQLite file and a folder of uploaded
firmware files. Design: `docs/SYSTEM_DESIGN.md` section 7.6.

## Where it runs (set up 2026-11-17)

Hostinger server `187.127.175.183`:

| What | Where |
|---|---|
| Code | `/opt/lpc-ota/lpc_ota.py` |
| Data | `/opt/lpc-ota/data/` (`ota.db`, `builds/<n>.hex`) |
| Settings | `/opt/lpc-ota/ota.env` (login name, password, port; readable by the service user only) |
| Service | `lpc-ota.service` (systemd, user `lpcota`, restarts by itself) |
| Units | port 80, paths under `/lpc/`, passed on by nginx (`location /lpc/` in `/etc/nginx/conf.d/spatika-health.conf`; the file before the change is `/root/spatika-health.conf.before-lpc-ota`) |
| Office page | port 5002, login `admin` and the password in `ota.env` |

The health app (port 80, 8000) and the BIH dashboard (port 5001) are separate and were not changed.

## Use (office page)

- **Unit type switch** (top of Stations, Firmware files and History): KSNDMC TRG, BIHAR TRG, KSNDMC TWS,
  each with its station count and how many need a look. Everything below it is that type only.
- **Live**: the Stations tab and each station's page redraw themselves (every 3 s while a station is updating,
  every 20 s otherwise): progress bar, KB done, speed and time left, and the update steps Sent, Downloading,
  Installing (ESP32: On trial), Updated. No reload is needed. A download that stops for the slot shows "paused".
- **Stations**: the type's totals at the top (stations, need a look, updating now, updated), then "Needs attention" for the type, then one row per station: firmware, battery, signal, records
  sent the last day (and how many by backlog), last check, update state. Click a station for its page:
  latest health report, firmware update control, health history, events.
- **Send an update** (on each type's tab): choose the firmware, tick the stations, Send. "Every station of this
  type" also covers stations that check in later. Try one station first.
- **CTRL firmware** (v6.9.63): upload `ctrl.hex` on Firmware files (listed as "CTRL, every type"), then choose
  it in Send an update, or on a station's page under CTRL firmware. A station takes it only after any main-board
  update, and only with main firmware v6.9.63 or later; its row shows a second, CTRL pill.
- **Firmware files**: upload the file that goes on the SD card (`fw_ksn_trg.hex`, `fw_bihar.hex`,
  `fw_ksn_tws.hex`). It is refused unless it is a whole main-firmware file with the right header and CRC.
  Uploading sends it to nobody. It is filed under its own type (read from the file) and the page then shows
  that type. The list shows the chosen type's files only.
- **History**: the chosen type's events (a password change shows under every type).
- A station checks twice a day (00:30 to 02:15 and 12:30 to 14:15, by station number; v6.9.60 and earlier: 01:00 and 13:00), or after every delivered slot with `OTA.NOW`
  on its card. It appears on the page by itself at its first check (firmware v6.9.55 build 23 and later).

A station that went back to its old firmware is not offered the same build again: upload a new one.

## DWLR stations

DWLR (radar water-level) units are ESP32 stations too, from firmware DWLR-D15-S.13 (`DWLR/ota_esp.ino`):
types **ESP32 · DWLR · SPATIKA** (28) and **ESP32 · DWLR · KSNDMC** (30). Upload the DWLR build's `.bin`
on Firmware files; send it as for AIO9. They check at about 13:00 and 01:00 (every slot with `OTA.NOW` on
their SD card) and send no daily health report.

## Health report

ESP32 AIO9 stations send the same daily report from firmware 6.57 (with their 13:00 check, else the 01:00
one), with the same field names; their Health card, history and "needs attention" show as for LPC stations,
with their own battery limits (`ESP_BATTERY_RULES`: single Li-ion cell). They no longer use the Spatika
health server (Contabo).


Units from v6.9.60 add health fields to their daily check (`docs/SYSTEM_DESIGN.md` 7.7). From v6.9.62 the
report comes once a day (the 12:30-14:15 check) and covers the last whole MET day (08:45 to 08:30): records
sent of 96 and how many by backlog, day and night battery averages, lowest battery, errors logged and the
newest one. The fields, their
names on the page and the "needs attention" limits are `HEALTH_FIELDS` and `HEALTH_RULES` in `lpc_ota.py`:
change them there. Reports are kept 400 days.

## Update the server code

After the change is pushed, in the Hostinger browser terminal as root:

    ./deploy_now.sh            # the newest code on the branch
    ./deploy_now.sh 63fc70f    # or a given commit
    ./deploy_now.sh back       # undo the last update

It touches only `/opt/lpc-ota/lpc_ota.py` (keeping the old one as `lpc_ota.py.before`) and restarts only
`lpc-ota`; it checks the file compiles before installing it and says if it was already running that code.
Put the script in `/root` once:

    curl -fsSL -o /root/deploy_now.sh https://raw.githubusercontent.com/satishvspatika/AIO_LPC/v6.2.9-untested/ota_server/deploy_now.sh && chmod 700 /root/deploy_now.sh

The database gains its new table and columns by itself at the first start.

Do not open `ota.db` as root while the service runs (files made by root lock the service out).

## Change the password

On the **Password** tab (current password, then the new one twice, at least 8 characters). It is kept hashed in
`ota.db`; other logged-in browsers are logged out. `LPCOTA_PASSWORD` in `ota.env` is used only while no
password has been set on the page.

Forgotten password: on the server run
`sudo -u lpcota sqlite3 /opt/lpc-ota/data/ota.db "DELETE FROM settings WHERE key='admin_pw'"`; the
`ota.env` password works again (`grep PASSWORD /opt/lpc-ota/ota.env`).

## Test locally

    LPCOTA_DATA=/tmp/otatest LPCOTA_PORT=5099 LPCOTA_BIND=127.0.0.1 LPCOTA_PASSWORD=test python3 lpc_ota.py
