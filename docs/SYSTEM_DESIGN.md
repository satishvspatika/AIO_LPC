# AIO Telemetry Station: System Design, Memory Map, Data Flow and Test Record

State as of 2026-11-07 evening: LPC1768 firmware **v6.9.11** (installed 17:25, not yet committed), LPC845 firmware
**845-v1.3**, branch `v6.2.9-untested` (last firmware commit: v6.9.10 + 845-v1.3, `77aa26b`). v6.9.10 ran clean from 15:35 to
17:25 (8 of 8 slots); v6.9.11 is now on the overnight / 1-2 day run.

This document is the single place for how the system is built and why, and for what has been tested. Where something
has only been built or read in code, and not run on a unit, it says so. Build steps and toolchain setup are in
`README.md`.

---

## 1. Purpose and requirements

A battery and solar powered rain (TRG) or weather (TWS) station that sends a record every 15 minutes over 4G/2G and
never loses data. It replaces deployed ESP32 + A7672S units and older LPC units, and must match the ESP32's behaviour
and server formats.

Requirements set by the product owner, in force:

| Requirement | How it is met |
|---|---|
| The 15th-minute transmission is critical | Wake at every slot; the record is saved before any send; failed sends go to a backlog |
| Don't lose records; don't loop and drain the battery | Flash + SD copies, backlog, missed-slot fill, send time limits, restart-loop limit |
| Rain day is 08:45 to 08:30 next day | LPC845 indexes samples from 08:45; day total resets only on a real change of rain day |
| `CUM_RF` never dips within a rain day | Never-dip rule on the LPC1768 (TRG); rain count kept across LPC845 restarts |
| No invented or random values | Missed slots are filled only from measured samples, or marked as not measured (signal -112/-110) |
| Bucket 0.5 mm | `RF_BUCKET_RESOLUTION 0.50` |
| Firmware files stay on the SD card | One card serves many stations; files are never deleted by the unit |
| No cables in the field | Both chips update from the SD card; MCU-Link only once, at the factory |
| Keep it simple and reliable | Prefer small, testable changes; anything uncertain falls back to proven behaviour |
| Every fix must hold for all three configs | Each build is made for KSNDMC_TRG, BIHAR_TRG and KSNDMC_TWS |
| Don't delay data during an outage | No hourly backoff; the modem tries at every slot |
| Memory is tight | RAM and flash checked on every build; ESP32 code is not ported as-is |

---

## 2. Hardware

Two microcontrollers on separate boards joined by a cable (LPC845 board J4 to LPC1768 board J9).

| Part | Role |
|---|---|
| **LPC845** (Cortex-M0+, 64 KB flash, 16 KB RAM) | Always on. Keeps time (DS1338 RTC), counts rain tips, reads sensors and battery, stores 3 rain days of 15-minute samples, and switches the power rails for everything else |
| **LPC1768** (Cortex-M3, 512 KB flash, 32 KB main RAM) | Powered only at slots or when an operator wakes it. Runs FreeRTOS, the SIM7600 modem, the 16x2 LCD and 3x3 keypad, the SD card, and all sending and storage |
| **SIM7600** | 4G LTE with 2G fallback, SMS, GPS. Powered by its own rail |
| **DS1338** | RTC with CR2032 backup (BT1). The coin cell connects only to the DS1338's VBAT pin, so its voltage cannot be measured on this board revision |

LPC845 power rail outputs (active low, 0 = on), read as `(GPIO PIN0 >> 26) & 7`:

| Pin | Rail | Bit in that value |
|---|---|---|
| PIO0_26 `SHDN_3V8` | SIM7600 | bit 0 |
| PIO0_27 `SHDN_3V3` | LPC1768 | bit 1 |
| PIO0_28 `SHDN_5V` | LCD | bit 2 |

So `111` = everything off (asleep), `100` = LPC1768 and modem on with LCD off (unattended slot), `000` = all on.

Other LPC845 board facts: JP1 links RST_845 to RESET (PIO0_5), JP2 links -PGM_845 to PIO0_12 (ISP). J2 is the
LPC845 SWD connector; **plugging the MCU-Link into J2 resets the LPC845** (reset status 2), which clears its RAM.
Inputs exist for BAT1, BAT2, solar (PIO0_21) and door sense. The separate JP1 on the LPC1768 board is USB power.

LPC845 reset status values (`SYSRSTSTAT`): 1 power-on, 2 reset pin, 4 watchdog, 8 brown-out, 16 software.

---

## 3. Unit configurations

Chosen at build time with `UNIT_CFG` in `LPC1768/user_config.h`.

| Config | Server | Payload | SD update file | Notes |
|---|---|---|---|---|
| `KSNDMC_TRG` (0) | `rtdas.ksndmc.net` / 117.216.42.181:80 `/trg_gprs/update_data_sit_v3` | form fields, cumulative rain | `fw_ksn_trg.hex` | Test station 1001 |
| `BIHAR_TRG` (1) | `rtdasbmsk.spatika.net` / 164.100.130.199:8085 `/Home/UpdateTRGData` | JSON, 15-min and cumulative rain | `fw_bihar.hex` | Bench station 1923, test station 1003 |
| `KSNDMC_TWS` (2) | `rtdas.ksndmc.net` / 117.216.42.181:80 `/tws_gprs/update_tws_data_v3` | form fields, temp, humidity, wind | `fw_ksn_tws.hex` | No rain total; no fill of slots not measured. Test station 1002 |

The LPC845 firmware (`sensor.hex`) is the same for all three.

Only **BIHAR_TRG** has been run on the bench (BSNL SIM). KSNDMC_TRG and KSNDMC_TWS are built and pass the SD file
check for each release, but have not been run against their servers.

---

## 4. Software structure

### LPC1768 (`LPC1768/`)

| File | Purpose |
|---|---|
| `main.c` | Slot scheduler, slot run, missed-slot fill, never-dip rule, crash/stack/stuck restart logging, restart-loop limit, task creation |
| `sim7600_driver.c` | Modem power, registration, APN choice, HTTP over TCP (by name, by IP, then modem HTTP), SMS, GPS, time sync |
| `intermcu_protocol.c` | Parses LPC845 frames, sends commands, link statistics |
| `hal_power.c` | Rail and heartbeat commands to the LPC845 |
| `telemetry_formatter.c` | Server endpoints and payload formats per config |
| `storage_log.c` | SD card: debug log, day files, backlog, state files, `ERRORS.TXT` |
| `flash_log.c` | Record log in flash (sectors 25-29) |
| `flash_nvram.c` | Settings in flash (sector 1): station ID, location, last logged, calibration result |
| `errlog.c` | Error log in flash (sectors 9-11), copied to `ERRORS.TXT` |
| `fw_update.c`, `fw_app.c`, `fw_ctrl.c` | Firmware update of both chips from the SD card; watchdog |
| `ui_display.c` | LCD, keypad, screens, menu, calibration, status SMS, idle power saving |
| `boot/` | Bootloader, swap/journal core shared with the LPC845, layout header |

FreeRTOS 6.0.1. Since v6.9.8 the kernel (`tasks.c`, `list.c`, `queue.c`, `port.c`, `heap_2.c`) is compiled from
source by `build.sh` at `-O1`, the same level as the old prebuilt library. The prebuilt library ignored
`FreeRTOSConfig.h`, so stack overflow checking could not be turned on with it.

### LPC845 (`LPC845/`)

| File | Purpose |
|---|---|
| `main.c` | RTC, rain counting and debounce, sensors, 15-minute samples, rail control, frames to the LPC1768, command handling, crash handler, stack guard, rain count kept across restarts |
| `boot/boot_main.c` | LPC845 bootloader: swap, trial and rollback, watchdog start |
| `boot/fw845_recv.c`, `boot/fw845_app.c` | Receives a new image over the inter-chip link; trial confirm; watchdog feed |
| `app.ld`, `boot/boot.ld` | Linker layouts |

---

## 5. Memory architecture

### LPC1768 flash (512 KB)

| Address | Sectors | Size | Contents |
|---|---|---|---|
| 0x00000 | 0 | 4 KB | Boot stub (56 bytes) |
| 0x01000 | 1 | 4 KB | Settings (`flash_nvram`) |
| 0x02000 | 2-8 | 28 KB | Bootloader (uses 1.8 KB). Linker limit set to 0x7000 so it can never reach the error log |
| 0x09000 | 9-11 | 12 KB | **Error log**: 48 pages of 256 B, a fixed ring |
| 0x0C000 | 12-15 | 16 KB | Firmware update journal |
| 0x10000 | 16 | 32 KB | Swap scratch |
| 0x18000 | 17-20 | 128 KB | **Application (slot A)**. v6.9.11 uses 118.4 KB (BIHAR_TRG), 118.6 KB (KSNDMC_TRG), 117.4 KB (KSNDMC_TWS); about 12 KB free |
| 0x38000 | 21-24 | 128 KB | Staging (slot B) for a new image |
| 0x58000 | 25-29 | 160 KB | **Record log**: 640 pages of 256 B, about 6.5 days of records |

`scripts/fw_check_hex.py` refuses any hex that writes settings (0x1000-0x2000), the error log (0x9000-0xC000), or
staging and the record log (0x38000-0x80000).

### LPC1768 RAM (32 KB main SRAM 0x10000000-0x10007FFF, plus 32 KB AHB SRAM 0x2007C000-0x20083FFF)

| Item | Size |
|---|---|
| FreeRTOS heap (`xHeap`, heap_2) | 16 KB, **in AHB SRAM at 0x2007C000 since v6.9.11**. Holds every task stack and queue. 4,128 B free after start-up |
| Modem receive buffers (`s_rx_buf`, `s_rx_temp`) | 2 KB each |
| Debug log ring buffer | 1,792 B (trimmed from 2,048 in v6.9.10) |
| HTTP request and capture buffers | 1 KB each |
| FatFs work areas | about 1.1 KB |
| **Free for the interrupt stack** | **about 16.8 KB** (variables end at 0x10003CB4) since v6.9.11; was 764 B in v6.9.10 |

The top 32 B of RAM are reserved for the IAP flash routines. RTC registers GPREG0-4 survive a reset and are used for
crash notes (GPREG0-1) and the fault restart count (GPREG4).

**AHB SRAM (v6.9.11).** Until v6.9.10 the second 32 KB block was unused (`.bss_RAM2` 0 bytes) while main RAM had
only 764 B left. v6.9.11 moves the heap there:
- `FreeRTOS_Library/portable/heap_2.c`: `xHeap` has `__attribute__((section(".bss.$RAM2")))`. The start-up code
  (`cr_startup_lpc17.c`) clears only the main `.bss`, so `prvHeapInit()` now clears the heap itself before first use;
  it starts from zero after every reset, as before.
- The USB-stick host driver was the only code that used this RAM (its buffers at 0x2007C000). It only ran for drive 1
  and the firmware mounts only drive 0 (the SD card); it is now off: `WITH_USB_MS 0` in `fat_sd/diskio.c`, and
  `fat_sd/ffconf.h` has one drive. Its code is still linked (kept by its interrupt vector) but never called. The unused
  UART `getline` helper also names 0x2007C000 and is never called.
- `hard_fault_capture` (`main.c`) saves the crash PC and LR only when the stack pointer is valid; it now accepts the
  AHB range too, since task stacks live there. Not yet forced on the unit (needs a crash test build).
- Upper 16 KB of AHB SRAM (0x20080000-0x20083FFF) remains free.

Task stacks (words of 4 bytes), with the lowest free seen on the bench:

| Task | Priority | Size | Lowest free | Notes |
|---|---|---|---|---|
| UiTask | 3 | 512 | 190 | |
| ModemTask | 2 | 512 (was 768) | 286 | |
| MainTask | 1 | 1088 (was 960) | 286 (was 160) | Runs the slots |
| DiskTask | 1 | 512 | 324 | Feeds the hardware watchdog; writes the error log |
| KbdTask | 3 | 160 | not measured | |
| Jobs: SMS, GPS, firmware update, test send | 1-2 | 896 (was 768) | SMS 118 (at 768), update about 450 | One at a time; refused safely if no memory. GPS not measured |

### LPC845 flash (64 KB, 1 KB sectors)

| Address | Sectors | Contents |
|---|---|---|
| 0x0000 | 0-3 | Bootloader (2.2 KB of 4 KB) |
| 0x1000 | 4-7 | Update journal (64-byte records, 64 pages) |
| 0x2000 | 8 | Swap scratch |
| 0x2400 | 9-32 | **Application (slot A)**, 24 KB. 845-v1.3 uses about 18.8 KB |
| 0x8400 | 33-56 | Staging (slot B) |
| 0xE400 | | Free to 60 KB |

Image info at slot base + 0xC0; vector 8 points to it (patched by `scripts/fw_image.py --lpc845`). The CRP word at
0x2FC is checked by `fw_check_hex.py --lpc845`, as some values lock the chip for good.

### LPC845 RAM (16 KB)

| Range | Contents |
|---|---|
| 0x10000000-0x100024FC | Application variables (data + bss), including 3 rain days of samples |
| 0x100024FC-0x10002514 | `.noinit`: the kept rain count (magic, slot tips, day tips, rain day, restored flag, check value) |
| Above that to 0x10003FE0 | Stack, filled with 0xA5A5A5A5 at start-up; deepest use seen about 1.1 KB of 6.9 KB |
| 0x10003FE0-0x10003FFF | Reserved for IAP flash routines |

The LPC845 bootloader uses RAM only up to 0x1000082C, so it cannot overwrite the kept rain count.

---

## 6. Inter-chip link

UART at 9600 baud, LPC1768 UART3 to LPC845 USART0.

**LPC845 to LPC1768, once a second:** a `$D` status frame with present time, last record time, last 15-minute tips,
battery ADC, temperature, humidity, wind speed and direction, live tips, day tips, rail states, wake reason, reset
status, firmware version and a checksum. Also the replies to the commands below.

**LPC1768 to LPC845:**

| Command | Meaning |
|---|---|
| `$H;` | Heartbeat. The LPC845 cuts all power after 11 minutes without one |
| `$f<n>;` / `$n<n>;` | Rails off / on (bit 1 LPC1768, 2 modem, 4 LCD). `$f7;` powers everything off (sleep) |
| `$S<time>;` | Set the RTC |
| `$G<time>;` | Get the stored sample for that slot (missed-slot fill) |
| `$C` / `$c` | Calibration start / stop |
| `$R;` | Erase data: restart the rain-day total |
| `$T<tips>;` | Return the stored day total after an LPC845 power loss (adds once only) |
| `$UQ`, `$UB`, `$UD`, `$UE`, `$UR`, `$UC`, `$UK` | LPC845 firmware update: query, begin, data, end, restart, confirm, reported |

---

## 7. Design flow

### 7.1 A 15-minute slot (unattended)

1. **Boundary.** The LPC845 stores the slot's tips and battery in its day samples (`store_data`), then switches on the
   LPC1768 and modem rails (LCD stays off).
2. **+0 to +5 s.** The LPC1768 boots and checks that LPC845 frames are arriving (5 s). With no frames it stores and
   sends nothing, so a record of zeros can't be made; the slot is filled later.
3. **Missed-slot fill.** Slots missed while off are rebuilt from the LPC845's stored samples (signal -104) or, if the
   LPC845 was off too, filled as not measured (-112 this rain day, -110 an earlier one) on TRG only.
4. **Hold to +62 s.** Servers refuse a record sent less than 62 s after its boundary. The hold counts all the time
   since the slot began (fixed in v6.9.2).
5. **Never-dip check (TRG).** `CUM_RF` is raised to the newest stored record's total if it would drop.
6. **Modem on and registered.** Up to 2 tries; 1 try after about an hour with no network. A missing SIM stops after
   one CFUN cycle.
7. **Save first.** The record goes to flash, then to the SD day file (the last row is compared first, so a restart
   can't add a second copy).
8. **Send.** Up to 3 tries, one per route (by name, by IP, by modem HTTP). The route that worked then carries up to 15
   backlog records. No new send starts after 300 s.
9. **Not delivered?** The record goes to the SD backlog, or to the flash queue if there is no card.
10. **GPS** (01:00, 07:00, 13:00, 19:00 with no saved position; 13:00 only once a position is saved; skipped below
    11.8 V), then a check for incoming SMS.
11. **Finish.** `SLOTS.CSV` line, stack margins logged, last slot saved, then `$f7;` to power off.

Measured on the bench: about **71 s** on-time for a normal slot, about **92 s** with no SIM, about **170 s** at a GPS
slot with no fix.

### 7.2 Operator use

DSP wakes the unit (LPC845 key wake). The LCD stays on until 90 s after the last key, or after the end of a job
(status SMS, GPS, update). Jobs still running after 8 minutes are cancelled, and the unit restarts if that fails.
Calibration: 5-minute run decided at 5 minutes (exactly 10 tips is PASS), then 1 minute to read, so about 6 minutes.
Power-off is refused during a slot, a job, a firmware update, and in the 5 s before a boundary. A slot that comes
while the LCD is on runs normally and is marked `OPERATOR` in `SLOTS.CSV`.

### 7.3 Screens and menu (v6.9.6 onward)

Scroll (read-only; EN does nothing): Station ID landing page, date/time, rainfall (TRG) or temp/humidity and wind plus
solar (TWS), battery, signal, location, last logged, backlog, field calibration result (TRG), and **POWER SAVE**,
which is one UP from the landing page (EN there powers off).

MENU (actions): SEND STATUS SMS, GPS UPDATE, KEY TEST, ERASE DATA, FIRMWARE UPDATE, SET STATION ID. BACK from an
action returns to MENU. The CALIB key starts calibration directly on TRG. DSP opens the power save prompt. SEND HEALTH
will go second in the menu when built. `TEST SEND` is switched off (`ENABLE_TEST_SEND 0`).

Landing page: config name centred on line 1; line 2 `ID:1923  v6.9.11`, with the version always shown in full.

### 7.4 Time

The server's `Date:` header is checked on every delivered record and the RTC corrected if it is 90 s or more off
(45 s after a refused record). Tower time (`CCLK`) is used only if the RTC has no valid time; GPS also corrects it on
a good fix. With no valid clock a scheduled wake gives up after 3 minutes, or 2 with no network (v6.9.4). The bench
unit's clock reads 6-8 s behind the server and the Mac.

### 7.5 Firmware update from the SD card

LPC1768: MENU > FIRMWARE UPDATE copies the config's file (`fw_bihar.hex` etc.) to staging, checks it, restarts into
the bootloader, which swaps it in and gives it 3 boots under a 5-minute watchdog. The new image confirms itself after
30 s with good LPC845 frames, or is rolled back. `REVERT F/W` goes back to the previous image.

LPC845 (CTRL F/W): the LPC1768 sends `sensor.hex` over the link (about 60 s, about 330 B/s), the LPC845 checks it,
swaps it in (about 9 s) and runs it on trial until the LPC1768 confirms it with `$UC`. A hang on trial is restarted
by the bootloader's 5.3-minute watchdog and rolled back. Updates are refused within 3 minutes of a slot. The LPC845
restart drops the rails, so the LPC1768 restarts too; the rain count is kept (845-v1.3).

Outcomes are written to `UPDATE.TXT`.

---

## 8. Data storage

Every record goes to **flash and SD**. The card is the second copy; with no card everything runs from flash.

SD card files:

| File | Contents |
|---|---|
| `DATA/<station>_<YYYYMMDD>.TXT` | Records for one rain day, named by the date it closes on (`NN,date,time,rf15,cum,signal,battery`) |
| `BACKLOG.DAT` | Records waiting to be sent (each 401 bytes, sent flag) |
| `SLOTS.CSV` | One line per slot: time, slot, result, reason, tries, signal, seconds, battery, boot type, backlog |
| `UPDATE.TXT` | Firmware update outcomes |
| `ERRORS.TXT`, `ERRORS.OLD` | Copy of the flash error log; rolls at 64 KB, so at most 128 KB |
| `REJECTED.TXT` | Records the server refused 3 times |
| `LOG/<YYMMDD>.TXT`, `HTTPLOG.TXT`, `DEBUG.TXT` | Debug logs (about 1 MB a day). To be switched off in production |
| `LASTSLOT.DAT`, `LASTLOG.DAT`, `FLASHSYN.DAT`, `ERRSYNC.DAT`, `RATMODE.DAT`, `SDCHECK.DAT` | State |
| `fw_bihar.hex`, `fw_ksn_trg.hex`, `fw_ksn_tws.hex`, `sensor.hex` | Firmware files, never deleted by the unit |

Flash record log: each page holds the SD record and the HTTP payload (up to 167 characters; Bihar's is about 140).
Outage start and end are marked, so after a card failure the unit can work out which records never reached the
server and queue them again. When a card comes back, flash copies across what it missed, up to 96 records a slot.
With no card and no network, the oldest records are overwritten after about 6.5 days.

---

## 9. Protection against faults

| Fault | LPC1768 | LPC845 |
|---|---|---|
| Crash (hard fault) | Location saved in RTC registers (task stacks in AHB SRAM accepted since v6.9.11), restart, logged at boot | Restart at once (845-v1.2) |
| Stack overflow | FreeRTOS check on every task switch; task name saved, restart, logged (v6.9.8) | Pattern at the stack's edge checked every 100 ms (845-v1.2) |
| Task stops responding | Main, UI and Modem check in; a task silent for 360 / 60 / 180 s is logged, then restart (v6.9.9) | Main loop must feed the watchdog |
| Everything stops | 60 s hardware watchdog, fed only by the disk task | 60 s watchdog once running normally; 5.3 min on a firmware trial |
| LPC1768 hangs with power on | LPC845 cuts all power after 11 min with no heartbeat | |
| Repeated fault restarts | After 3 in a row, powers off until the next slot (v6.9.9) | |
| Restart loses rain | | Slot tips, day total and its rain day kept in `.noinit` across crash, watchdog and update restarts (845-v1.3). Power loss falls back to `$T` |
| LPC845 silent | No record made (no zeros); slot filled later | |
| `CUM_RF` would drop | Held at the stored total (TRG) | Day total can't carry past 08:45 after a restart |
| Restart mid-slot | Slot runs again without a second stored copy; sent again (the server may see a repeat) | |
| Bad clock | Gives up after 3 min (2 with no network) | |
| No SIM / no network | Record saved to backlog; 1 try a slot after about an hour | |
| Stuck job (SMS, GPS, test) | Cancelled at 8 min, restart if that fails | |

**Error log.** Every `ERROR` line also goes to flash (sectors 9-11), with the version and time: at most 8 writes a
boot, repeats counted instead of rewritten, only one entry waiting at a time. The disk task writes it within a second
and copies new entries to `ERRORS.TXT` every 30 s. It survives power loss, firmware updates and ERASE DATA. Before the
restart-loop power-off, the unit waits for each message to be written (v6.9.10).

---

## 10. Test record

All on the bench unit: BIHAR_TRG, station 1923, BSNL SIM, bench power supply (battery readings 13.3-13.4 V; about
0.1 V lower with the LCD on). Unless noted, results were checked on the card and on the server.

### 2026-11-06: LPC845 bootloader and SD update

| Test | Result |
|---|---|
| Hosted: swap/rollback simulation, every power-cut point | Pass: 4,619 (LPC1768) and 4,914 (LPC845) scenarios, no image lost |
| Hosted: LPC845 transfer protocol, 40 updates in a row | Pass |
| LPC845 update from SD to 845-v1.1 | Pass (`845 UPDATE OK`); `$T` restored the total within 2 s |
| Transfers stopping at random points ("845 NO ANSWER") | Found and fixed: the LPC845 UART interrupt wrote while the transmitter was full |
| Rollback from hanging firmware 845-BAD | Pass: hang from 22:40, recovered 22:56:35, rolled back |
| No zero records while the LPC845 is silent | Pass on rerun: 22:45 filled (-112, CUM_RF 2), 23:00 normal. The first run had sent false zeros at 21:45-22:15 (fixed) |
| Overnight run | 40 of 40 slots first try; 96 of 96 records for the rain day; 0 damaged frames |
| 08:45 rain-day change | Pass: total to 0, new day file |

### 2026-11-07: robustness work

| Version | Test | Result |
|---|---|---|
| v6.9.0 | Never-dip: LPC845 total forced to 0 by probe | Pass: `CUM_RF would have dropped to 00.00 mm, keeping 01.00 mm`; server showed 1 |
| 845-v1.0 | Final LPC845 firmware installed by SD | Pass |
| v6.9.1 | Idle off at 90 s | Pass |
| v6.9.1 | Calibration PASS (10 tips) / FAIL (7 tips) at exactly 5:00; power-down at 6:02 | Pass |
| v6.9.1 | Status SMS keeps the LCD on, then off about 90 s after it ends | Pass |
| v6.9.1 | Slot with the LCD on (09:45, 10:15) | Pass, marked OPERATOR |
| v6.9.1 | Calibration tips kept out of the rain total | Pass |
| v6.9.2 | Send at +62 s instead of +67 s (10:45, 11:00, 11:15) | Pass: accepted first try, about 71 s on-time |
| v6.9.2 | No SIM for 3 slots, tips given, SIM back | Pass: 3 records held (-113), all sent with the next slot in 21 s, values exact, no duplicates; no-SIM slot 92 s |
| v6.9.3/4 | LPC1768 power cut 8 s, 66 s into a slot | Before the last-row check: 2 rows on the card. v6.9.4: 1 row, delivered on the rerun |
| v6.9.4 | Card out during a slot | Record kept in flash; card caught up next slot |
| v6.9.6 | Read-only scroll and action menu | Pass (checked by hand) |
| v6.9.8 | Status SMS on the new kernel build | Pass; first SMS took 65 s on 2G |
| 845-v1.2 | Forced crash, stack damage, hang (by probe) | Pass: restart 0.1 s, 0.1 s, 60 s (watchdog) |
| 845-v1.3 | 3 tips, then forced crash and hang | Pass: tips kept through both; 14:45 record 1.5 mm, CUM_RF 7.5 |
| v6.9.9T/U | Forced stack overflow in UI (test build) | Pass: `RESTARTED AFTER STACK OVERFLOW in task UiTa` |
| v6.9.9T/U | UI task stopped | Pass: restart after 62 s, logged |
| v6.9.9T/U | All tasks stopped | Pass: hardware watchdog restart, logged |
| v6.9.9T/U/10T | Third fault restart in a row | Pass: powered off; message in `ERRORS.TXT` from v6.9.10 |
| v6.9.9 | Flash error log copied to `ERRORS.TXT` | Pass, with version and time |
| v6.9.10 | Long run 15:35-17:25 | Pass: 8 of 8 slots first try (71-73 s), no restarts, no new errors; stacks steady (Ui 190, Modem 286, Main 276-286, Disk 346-348 words), heap 4,128 B |
| v6.9.11 | All three configs build; heap at 0x2007C000; main RAM free about 16.8 KB | Pass (map checked) |
| v6.9.11 | Install by SD (MAIN F/W) at 17:25 | Pass: trial, confirmed healthy at +30 s |
| v6.9.11 | First slot 17:30, with 1 tip | Pass: sent first try, 0.5 mm, CUM_RF 9.0 to 9.5; heap 4,128 B; stacks same as v6.9.10; 0 damaged frames. 85 s on-time: slow network (CIPOPEN 6 s, server reply 6 s), not firmware |
| all | Hosted tests rerun before each commit | Pass |

### Not tested yet

- Bad-clock wait (v6.9.4 change)
- GPS job stack margin, and GPS fix at all (no fix seen, even with the antenna)
- Damaged-frame count comparison (no damaged frames occurred)
- KSNDMC_TRG and KSNDMC_TWS on a unit and server
- Real battery drain; long outages; full card; power cuts during settings save and firmware swap on the real unit
- The 1-2 day run of v6.9.11 + 845-v1.3 (in progress since 17:25)
- Crash capture with the stack in AHB SRAM (needs a forced hard fault in a test build)

---

## 11. Known issues and open items

| Item | Status |
|---|---|
| GPS never gets a fix | Open. Every search is a cold start of 90 s |
| Blank LCD seen once | Unexplained |
| `CIPOPEN` timeouts | To review |
| Status SMS marked "Flag Invalid" by the server | Accepted for now |
| RAM | Fixed in v6.9.11: heap in AHB SRAM, about 16.8 KB main RAM free (was 764 B). Confirm on the long run |
| SMS on 2G can take over a minute | Watch (power) |
| Bihar SMS number | Temporary 9980945474; the ESP32 uses 9741391102 |
| RTC coin cell | Can't be measured on this board; firmware can only detect a stop. ADC link proposed for the next board |

---

## 12. Plan

**Done today:** stack, watchdog, error log, LPC845 protection, rain count kept, RAM relieved (v6.9.11). v6.9.11 on
the 1-2 day run; commit and tag it proven if clean.

**Next batches**, each tested before the next:

1. Handling failures: modem and SD lockups, `$f7` ignored, data sanity (out-of-range values, rain during a bad clock),
   dead LPC845 link report and fill, bad-clock test.
2. Power cycling: power cuts during every kind of write, card loss, full card.
3. Power and network flow: SIM check first (a no-SIM slot to about 20-30 s), recovery by failure type (CFUN, rescan,
   stop early on denied), roaming (clear blocked networks, try 4G once a day), then adaptive wake (measure first).

**Then:** KSNDMC_TRG on the bench (after the long run), then KSNDMC_TWS last (needs sensors); health report twice a
day (01:00 and 13:00) with SEND HEALTH; OTA through the health server with a signature check; production settings
(debug log switch, Bihar SMS number, solar on TRG); real-battery test; factory programming script; merge to `main`.

**On hold:** hourly backoff in outages; GPS power reduction until GPS works; old-unit fallback mode; ESP32 test jig
(separate project).

---

## 13. Bench methods

The scripts are in `bench/`, listed in `bench/README.md`.

- **Toolchain:** `arm-none-eabi-*` from `../LPC/TWS6/tools/arm-toolchain/bin`. Builds: `LPC1768/build.sh`,
  `LPC845/build.sh`. The other configs are built by changing `UNIT_CFG` in a copy of the tree.
- **Probe:** MCU-Link on J2 (LPC845) with pyOCD (`pyocd`, Python API `session_with_chosen_probe(target_override=
  'lpc845', connect_mode='attach')`). Attaching does not reset the chip; plugging the cable in does.
- **What the probe was used for:** reading `rain_day_tips`, `rf_val`, `reset_status`, rails and the update journal;
  forcing the day total to 0 (never-dip test); setting `wake_key_pressed` to wake the unit; cutting the LPC1768 rail
  (GPIO SET 0xA0002200 / CLR 0xA0002280, bit 27) to simulate a power cut mid-slot; forcing a crash (PC to 0xF0000000),
  a hang (PC to a branch-to-self in RAM) and stack damage; timing restarts by writing a marker into `wake_reason`.
- **LPC1768 fault tests:** a build with `TEST_FAULTS` makes KEY TEST force faults: UP overflows the UI stack, DOWN
  stops the UI task, RIGHT stops every task. Never ship it.
- **SD card on the Mac:** mounts as `/Volumes/NO NAME`; copy with `cp -X`, check with `cmp`, then `diskutil eject`.
  Take the card out only when the unit is asleep and not within 2 minutes after a boundary.
