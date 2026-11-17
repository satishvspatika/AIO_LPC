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

## Use

1. Upload the firmware file that would go on the SD card (`fw_ksn_trg.hex`, `fw_bihar.hex`, `fw_ksn_tws.hex`).
   It is refused unless it is a whole main-firmware file with the right image header and CRC.
2. Choose who gets it: one station ("Gets" in the Stations table), or every station of a type ("Whole type").
   Try a build on one station first.
3. The station asks once a day (13:00 to 14:45, by station number), downloads, installs, and reports at its
   next slot. The Stations table shows the version it runs and the result; "Recent events" keeps the history.

A station that went back to its old firmware is not offered the same build again: upload a new one.

## Update the server code

    scp ota_server/lpc_ota.py root@187.127.175.183:/opt/lpc-ota/lpc_ota.py
    ssh root@187.127.175.183 'chown lpcota:lpcota /opt/lpc-ota/lpc_ota.py && systemctl restart lpc-ota'

Do not open `ota.db` as root while the service runs (files made by root lock the service out).

## Change the password

Edit `LPCOTA_PASSWORD` in `/opt/lpc-ota/ota.env`, then `systemctl restart lpc-ota`.

## Test locally

    LPCOTA_DATA=/tmp/otatest LPCOTA_PORT=5099 LPCOTA_BIND=127.0.0.1 LPCOTA_PASSWORD=test python3 lpc_ota.py
