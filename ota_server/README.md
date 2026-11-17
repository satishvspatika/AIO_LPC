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

- **Stations**: "Needs attention" first (every type), then a tab per station type with a count, one row per
  station: firmware, battery, signal, slots on time, last check, update state. Click a station for its page:
  latest health report, firmware update control, health history, events.
- **Send an update** (on each type's tab): choose the firmware, tick the stations, Send. "Every station of this
  type" also covers stations that check in later. Try one station first.
- **Firmware files**: upload the file that goes on the SD card (`fw_ksn_trg.hex`, `fw_bihar.hex`,
  `fw_ksn_tws.hex`). It is refused unless it is a whole main-firmware file with the right header and CRC.
  Uploading sends it to nobody.
- A station checks twice a day (00:30 to 02:15 and 12:30 to 14:15, by station number; v6.9.60 and earlier: 01:00 and 13:00), or after every delivered slot with `OTA.NOW`
  on its card. It appears on the page by itself at its first check (firmware v6.9.55 build 23 and later).

A station that went back to its old firmware is not offered the same build again: upload a new one.

## Health report

Units from v6.9.60 add health fields to their daily check (`docs/SYSTEM_DESIGN.md` 7.7). The fields, their
names on the page and the "needs attention" limits are `HEALTH_FIELDS` and `HEALTH_RULES` in `lpc_ota.py`:
change them there. Reports are kept 400 days.

## Update the server code

From the server (Hostinger browser terminal), after the change is pushed; `<commit>` is its hash:

    cp /opt/lpc-ota/lpc_ota.py /opt/lpc-ota/lpc_ota.py.before && curl -fsSL -o /tmp/lpc_ota.py https://raw.githubusercontent.com/satishvspatika/AIO_LPC/<commit>/ota_server/lpc_ota.py && python3 -m py_compile /tmp/lpc_ota.py && install -o lpcota -g lpcota -m 644 /tmp/lpc_ota.py /opt/lpc-ota/lpc_ota.py && systemctl restart lpc-ota && systemctl is-active lpc-ota

Back: `cp /opt/lpc-ota/lpc_ota.py.before /opt/lpc-ota/lpc_ota.py && systemctl restart lpc-ota`.
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
