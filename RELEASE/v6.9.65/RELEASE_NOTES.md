# AIO Data Logger: Release v6.9.65

**Release date:** 22 November 2026

**Type:** Production build (the debug log is off unless the card holds `DEBUGLOG.ON`)

**For:** Test team, bench and field test units. Replaces v6.9.64, which was never deployed.

| Part | Version | File | Folders |
|---|---|---|---|
| Main firmware, KSNDMC rain gauge (TRG) | v6.9.65 | `fw_ksn_trg.hex` | OTA, SD_CARD |
| Main firmware, Bihar rain gauge (TRG) | v6.9.65 | `fw_bihar.hex` | OTA, SD_CARD |
| Main firmware, KSNDMC weather station (TWS) | v6.9.65 | `fw_ksn_tws.hex` | OTA, SD_CARD |
| CTRL firmware (all unit types) | v1.17 (same as v6.9.63) | `ctrl.hex` | OTA, SD_CARD |
| Old-unit conversion image | MIG-1.10 (same as v6.9.63) | `fw.hex` | SD_CARD only |
| Factory images (bootloader + firmware, MCU-Link) | v6.9.65 / CTRL v1.17 | `MAIN_<type>_v6.9.65_FULL.hex`, `CTRL_v1.17_FULL.hex` | FACTORY_MCU-LINK |

## Folders

| Folder | Used for |
|---|---|
| `OTA/` | Upload to the office page. The three main files plus `ctrl.hex` (the office sends CTRL firmware only when no main update is waiting). |
| `SD_CARD/` | Copy to the root of a unit's card for a keypad update, or with `fw.hex` to convert an old unit. |
| `FACTORY_MCU-LINK/` | A blank or bricked board, flashed with MCU-Link. |

`fw.hex` is not in `OTA/`: it is only for old units with the original SD bootloader, which cannot take an OTA update.
It does not carry the main firmware: it installs the new bootloaders and CTRL, then the unit takes `fw_*.hex` from
the same card.

---

## 1. What is new since v6.9.63

### OTA: no update can leave a unit silent
- **New firmware must deliver a record within 2 hours (8 slots)** of its install, or the unit goes back to the
  previous firmware. Before, firmware that ran for 30 s but could no longer reach the server was kept for good.
- **A damaged download is fetched again**, at most 3 times (bad line, gap, short file, image check, card read),
  instead of that firmware being refused for good.
- **The office page says why.** The unit tells the office why a download is waiting (battery below 11.0 V, or
  records still to send) and why an update was given up: no record in 2 h, did not stay, for another unit type,
  not a good image, damaged 3 times, flash write failed. Before, a waiting download showed as "Starting download".

### Flash room
About 2.5 KB of flash freed (BIHAR had 148 bytes left), with no change in what the unit does:

| Change | Bytes |
|---|---|
| SD card names: conversion for English letters only (`fat_sd/option/ascii.c` replaces `ccsbcs.c`). Every name the firmware uses is ASCII; a non-ASCII name a PC leaves on the card is listed by its 8.3 name. | ~1,250 |
| No C-library heap: sscanf/snprintf only named malloc/realloc/free for cases this firmware never reaches | ~650 |
| CTRL update through the ROM loader sets the clock with one division instead of `SystemCoreClockUpdate()` | ~550 |

| Type | Image (slot 131,072) | Free | CRC32 |
|---|---|---|---|
| BIHAR | 128,456 | 2,616 | 0xE9F8DA72 |
| KSNDMC TRG | 128,144 | 2,928 | 0xDC999A73 |
| KSNDMC TWS | 127,904 | 3,168 | 0x66A37EAB |

### Office page (server, deployed separately with `deploy_now.sh`)
- What a station is doing with an offered update, in words, with the next check time.
- Health history: battery, records sent of 96, signal, errors/restarts charts, and up to 90 daily reports.
- A station silent for more than 26 h shows what its last report said and the likely cause.
- Events reworded; only the last 10 kept per station.

---

## 2. How to install

### Over the air (units on v6.9.55 or later)
1. On the office page, upload the `OTA/` file for the station's type and set it for the station.
2. CTRL: upload `OTA/ctrl.hex` and set it after the main firmware has updated (only if the station is not on v1.17).

### Units already on v6.9.x, by SD card
1. Copy everything from `SD_CARD/` to the root of the card (replace the old files). For test units, also create an
   empty `DEBUGLOG.ON`.
2. **MENU > FIRMWARE UPDATE > CTRL F/W** if the LCD does not show CTRL v1.17; wait for `CTRL UPDATE OK`.
3. **MENU > FIRMWARE UPDATE > MAIN F/W.** `UPDATE.TXT` shows `UPDATE OK, now running v6.9.65`.

### Old units (original firmware with the old SD-card bootloader)
1. Copy `SD_CARD/` to the root of a card.
2. With the unit **off**, fit caps on **JP1 and JP2**, insert the card, power on.
3. The LCD shows `CONVERTING UNIT  MIG-1.10`. At `REMOVE JP1+JP2`, take both caps off. The unit starts on v6.9.65.
4. Check the station ID and unit type on the LCD.

### Blank or bricked boards
Flash `FACTORY_MCU-LINK/MAIN_<type>_v6.9.65_FULL.hex` to the LPC1768 and `CTRL_v1.17_FULL.hex` to the LPC845.

---

## 3. What to test (first run on units with this release)

| # | Test | Expected |
|---|---|---|
| 1 | OTA from v6.9.63 to v6.9.65, each type | `UPDATE OK`; office page shows "Updated to v6.9.65: running and confirmed" |
| 2 | Offer an update with the battery below 11.0 V | Office page: "Waiting: battery low" with the volts |
| 3 | Pull the modem antenna right after an OTA install, for over 2 h | Unit goes back to the previous firmware; office page says "delivered no record in the 2 hours…" |
| 4 | Normal day, card in all the time | Every slot `SENT` in SLOTS.CSV; LOG and DATA written as before |
| 5 | SD-card update from `fw_ksn_trg.hex` (long name) | Found and installed as before |
| 6 | CTRL update by keypad and by OTA | `CTRL UPDATE OK`; LCD and office page show v1.17 |
| 7 | Day after install | Health report on the office page, with the Health history charts |

---

## 4. Checks done before release

- All three types built from the same source; OTA and SD_CARD files are byte-identical; the office parser reads
  the type and version of each main file and of `ctrl.hex` (CTRL v1.17).
- Host tests: ota_test 83,418, ota_get_test 900, health_test 381 checks, all pass.
- **Not yet run on a unit.**
