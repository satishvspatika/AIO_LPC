# AIO Telemetry Station: System Design, Memory Map, Data Flow and Test Record

State as of 2026-11-08 afternoon: LPC1768 firmware **v6.9.13**, LPC845 firmware **845-v1.5** (`446ff19`), branch
`v6.2.9-untested`. Two units: BIHAR_TRG 1923 (bench, v6.9.12 + 845-v1.5 since 09:00) and KSNDMC_TRG 1922 (v6.9.13 +
845-v1.5 since 13:10, now on a BSNL SIM).

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
| `KSNDMC_TRG` (0) | `rtdas.ksndmc.net` / 117.216.42.181:80 `/trg_gprs/update_data_sit_v3` | form fields, cumulative rain | `fw_ksn_trg.hex` | Second unit 1922 (Airtel IoT SIM), test station 1001 |
| `BIHAR_TRG` (1) | `rtdasbmsk.spatika.net` / 164.100.130.199:8085 `/Home/UpdateTRGData` | JSON, 15-min and cumulative rain | `fw_bihar.hex` | Bench station 1923, test station 1003 |
| `KSNDMC_TWS` (2) | `rtdas.ksndmc.net` / 117.216.42.181:80 `/tws_gprs/update_tws_data_v3` | form fields, temp, humidity, wind | `fw_ksn_tws.hex` | No rain total; no fill of slots not measured. Test station 1002 |

The LPC845 firmware (`sensor.hex`) is the same for all three.

**BIHAR_TRG** runs on bench unit 1923 (BSNL SIM). **KSNDMC_TRG** runs on unit 1922 (Airtel IoT SIM) since 2026-11-07
18:04, sending to the KSNDMC server. KSNDMC_TWS is built and passes the SD file check for each release, but has not
been run on a unit.

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
| 0x18000 | 17-20 | 128 KB | **Application (slot A)**. v6.9.13 uses 119,160 B (BIHAR_TRG), 119,332 B (KSNDMC_TRG), 118,108 B (KSNDMC_TWS); about 12 KB free |
| 0x38000 | 21-24 | 128 KB | Staging (slot B) for a new image |
| 0x58000 | 25-29 | 160 KB | **Record log**: 640 pages of 256 B, about 6.5 days of records |

`scripts/fw_check_hex.py` refuses any hex that writes settings (0x1000-0x2000), the error log (0x9000-0xC000), or
staging and the record log (0x38000-0x80000).

### LPC1768 RAM (32 KB main SRAM 0x10000000-0x10007FFF, plus 32 KB AHB SRAM 0x2007C000-0x20083FFF)

| Item | Size |
|---|---|
| FreeRTOS heap (`xHeap`, heap_2) | **20 KB since v6.9.15** (16 KB from v6.9.11), in AHB SRAM at 0x2007C000. Holds every task stack and queue. 4,128 B free after start-up on v6.9.14; about 6.9 KB expected on v6.9.15 (modem stack 832 words); a job task (896 words) needs about 3.7 KB |
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
- From v6.9.15 the heap is 20 KB (to 0x20080FFF); 0x20081000-0x20083FFF (12 KB) remains free.
- **Flash writes must take their data from main RAM (v6.9.18).** The boot ROM's copy-to-flash refused a source at or
  across 0x20080000 (the second AHB block). With the 20 KB heap of v6.9.15-v6.9.17, the update task (created when an
  update starts) gets its stack at 0x2007F538-0x20080338, so its first frames, holding the 256-byte page and the
  journal record, are above that address: v6.9.15 showed "UPDATE STOPPED FLASH ERROR" on 1922 (2026-11-08 evening),
  and revert, which writes a journal record from the same task, would fail the same way. Under v6.9.14 the whole
  stack was below 0x20080000 (0x2007F040-0x2007FE40), which is why the update to v6.9.15 worked. v6.9.18 copies the
  data into a static main-RAM buffer in every flash write (`fw_hal_program`, flash log, error log, settings) and logs
  the IAP status code when staging fails. Read back on 1922 through the probe: the staged v6.9.17 image was complete
  and valid; the frames put the page buffer wholly above 0x20080000 (writes worked) and the journal record across it
  (0x2007FFE0-0x20080104, the write that failed), so the boot ROM refuses a source that crosses the boundary. Units on v6.9.15-v6.9.17 run normally otherwise (their other flash writes
  come from tasks made at start-up, low in the heap), but need the probe on the LPC1768 to leave that version.

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
| 0x2400 | 9-32 | **Application (slot A)**, 24 KB. 845-v1.5 uses 19,516 B |
| 0x8400 | 33-56 | Staging (slot B) |
| 0xE400 | | Free to 60 KB |

Image info at slot base + 0xC0; vector 8 points to it (patched by `scripts/fw_image.py --lpc845`). The CRP word at
0x2FC is checked by `fw_check_hex.py --lpc845`, as some values lock the chip for good.

### LPC845 RAM (16 KB)

| Range | Contents |
|---|---|
| 0x10000000-0x10002510 | Application variables (data + bss), including 3 rain days of samples (845-v1.5) |
| 0x10002800-0x10002818 | `.noinit`: the kept rain count (magic, slot tips, day tips, rain day, restored flag, check value). **Fixed address from 845-v1.5**, so every later version finds it after an update restart; the linker stops the build if variables reach it. Before v1.5 it followed the variables and moved when they grew (v1.3 to v1.4 lost the count; `$T` restored it) |
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
   LPC845 was off too, filled as not measured (-112 this rain day, -110 an earlier one) on TRG only. A filled row
   needs a record from the same rain day (CUM_RF carried from it) or from the rain day just before (CUM_RF 0, as the
   total restarts at 08:45 and nothing was counted; v6.9.12). A rain day further on, or no record at all, stays
   empty (`NO_DATA_FOR_RAIN_DAY` in SLOTS.CSV). Rain 0 and the last record's battery in every filled row.
   If the LPC845's slot time is invalid (its clock lost, or not yet set after a restart), the slot is named from the
   LPC1768's clock rounded down to the quarter hour, and an ERROR is logged (v6.9.13). Before that only the server
   payload used the corrected time; SLOTS.CSV, the DATA file and backlog entries kept the LPC845's (e.g.
   `20000101001500`). The rain is still the count the LPC845 closed, so it may land one slot late, never lost.
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

Landing page: config name centred on line 1; line 2 `ID:1923  v6.9.13`, with the version always shown in full. The next screen (v6.9.13) shows both chips'
versions, named as the two updates: `MAIN  v6.9.13` / `CTRL  845-v1.5`. The LPC845's is read about 11 s after boot
(`----` until then, or if it never answers) and replaced at once after a CTRL F/W update.

### 7.4 Time

The server's `Date:` header is checked on every delivered record and the RTC corrected if it is 90 s or more off
(45 s after a refused record). Tower time (`CCLK`) is used only if the RTC has no valid time; GPS also corrects it on
a good fix. With no valid clock a scheduled wake gives up after 3 minutes, or 2 with no network (v6.9.4). The bench
unit's clock reads 6-8 s behind the server and the Mac.

**LPC845 RTC watch (845-v1.5).** The LPC845 reads the DS1338 (I2C0, PIO0_10/11) every 100 ms. On 2026-11-07 unit
1922 stopped sending after 19:15: the I2C0 master jammed (STAT 0x800, start requested, nothing pending, both bus lines
high) while the modem was on. 845-v1.3 ignored the driver's errors, so the time read as zeros (2000-00-00), the minute
never changed and no slot wake came. Since v1.5:
- Both transfers of every read are checked. On failure I2C0 is reset (CFG 0, then re-init) and the last good time is
  kept (`rtc_i2c_resets` counts these). `Set_Time` retries once after a reset.
- If the halt bit is set, or the seconds stop for 3 s, the clock is restarted (`rtc_kicks`).
- Safety net: if the time is invalid (year < 24) or unread for 60 s with the LPC1768 off, the LPC845 wakes it after
  15 minutes (`bad_clock_wakes`) so it can take tower or server time.
Jams come only while the unit is awake, about 1-7 per slot on 1922 (the modem is the likely source); none seen asleep.

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
| Bad clock | Gives up after 3 min (2 with no network) | Jammed I2C reset, stopped RTC restarted, wake after 15 min with no valid time (845-v1.5). Stuck/halted judged only from good reads (845-v1.6) |
| Modem hung | Between the two power-on tries the 3.8V rail is held off 3 s, so the modem really restarts (v6.9.14; before, it was switched straight back on) | |
| `$f7` lost | Sent again at 10, 20 and 30 s if still powered, unless a slot is due (then restart and run it). The UI task checks in while waiting (v6.9.14) | |
| Impossible values | 15-min rain over 100 mm logged as ERROR; value sent as read (v6.9.14). No battery range check: the battery may be 12 V or 3.3-3.7 V under the new power design, so its limits come later as a per-unit setting with the low-voltage cut-off | |
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

### 2026-11-07 evening to 2026-11-08: second unit, RTC jam, gap fill

Unit 1922 (KSNDMC_TRG, Airtel IoT SIM) unless noted.

| Version | Test | Result |
|---|---|---|
| v6.9.11 | KSNDMC_TRG programmed by MCU-Link (chip erase), station ID 1922 from the build default | Pass: 18:15-19:15 sent to the KSNDMC server |
| 845-v1.3 | Silence after 19:15 | Found: LPC845 I2C0 jam, time read as zeros, no slot wakes (see 7.4). A loose CR2032 was a separate problem |
| v6.9.11 | Clock restored after the silence | 19:30-20:45 filled as -112 with CUM_RF 6.5; 21:00 onward normal; no duplicate 21:30 |
| 845-v1.4 | Update by SD | Pass (`845 UPDATE OK`); kept count lost as `.noinit` moved, `$T` restored CUM_RF 6.5. Led to the fixed address |
| 845-v1.4 | RTC stopped with the halt bit (`rtc_lost845.py halt`) | Showed the real fault: I2C jammed 26 s after the rails came on, 78 restarts did nothing. Led to v1.5 |
| 845-v1.5 | Installed by MCU-Link at 21:28 | Pass: 13 kept tips survived |
| 845-v1.5 | I2C0 forced off 3 times (`rtc_lost845.py jam`) | Pass: 3 of 3 recovered, time never invalid |
| 845-v1.5 | RTC stopped at 00:14 with the halt bit | Pass: restarted at once, wake at 21:33:55, tower time set 21:34:28, no repeat send |
| v6.9.11 + 845-v1.5 | Overnight 21:45-09:00, probe watching the LPC845 | Pass: every slot sent first try (38 of 38 to 07:00); 43 natural jams cleared, clock never invalid, no safety wakes; stacks and heap steady; no new errors; 08:45 day total to 0 |
| v6.9.11 | GPS search at 01:00 and 07:00 | 90 s, no fix (indoors); slot takes 101 s instead of about 8 s |
| v6.9.12 | Gap-fill rule host test (11 cases, month and year ends, two-day gap, no history) | Pass |
| v6.9.11 | 1922: SIM out 07:15-08:45, then no power 08:47-12:44 | 7 slots kept as -113, 15 filled as -112 (same rain day), all delivered by 13:01 with the 12:45 and 13:00 slots. No-SIM slot 29 s. On the weak battery the LPC1768 never started (no boot line) and the probe got no answer; on the supply it started at once |
| v6.9.12 | 1922: operator start at 12:44:47, 13 s before a slot | Ran the 12:45 slot and 15 backlogs, then slept 78 s after the slot: 224 s awake, as designed (the idle count pauses during a slot) |
| v6.9.13 | Slot naming with an invalid LPC845 slot time (host test with the real function) | Pass: `20000101001500` at 21:09:45 named `20261107210000`; empty named from the clock; valid left alone |
| v6.9.13 | All three configs build; hosted update tests | Pass; variables end at 0x10003CCC (+24 B) |
| v6.9.13 | Installed on 1922 by SD; versions screen | Pass: `MAIN  v6.9.13` / `CTRL  845-v1.5` |
| v6.9.14 + 845-v1.6 | Built and host-tested only (not on a unit yet): slot naming from the start-of-slot clock (4 cases incl. a slot run past the next boundary); all three configs build (117,524 / 117,696 / 116,316 B after dropping the battery range check, about 1.5 KB smaller as `send_raw` is no longer copied into every caller); hosted update tests; static stack the same as v6.9.13 | Pass |
| v6.9.14 + 845-v1.6 | Installed on 1922 by SD (2026-11-08 ~16:00): versions screen MAIN v6.9.14, CTRL 845-v1.6 | Pass |
| 845-v1.6 | `rtc_lost845.py jam` on 1922, unit awake: 3 forced I2C0 failures each recovered within 2 s, time never read as zeros, `kicks=0` (no clock restart from failed reads) | Pass |
| 845-v1.6 + v6.9.14 | `rtc_lost845.py safety` on 1922 (first run on a unit): clock set to 2000-01-01 while asleep, safety wake 1 s later (LCD off), clock back to network time 35 s after the wake, unit back asleep; day tips kept | Pass |
| v6.9.14 | 16:15 slot after the tests: on 16:15:00, off 16:16:25; three I2C jams during the modem-on time recovered | Pass |
| v6.9.14 | 1922, 16:04-16:46: update and 845 update OK; log quiet (no CIPCLOSE or SMS-poll lines); safety wake at 16:07 ran the current slot (16:00, named from this clock), found it already stored, sent it again without a second row; day file rows 00-32 with no gap or repeat | Pass |
| v6.9.15 | Built and host-tested only (not on a unit yet): station ID parser on all three payload formats and bad inputs; all three configs build (117,984 / 118,156 / 116,776 B); hosted update tests; heap 20 KB (`.bss_RAM2` 0x5000 in the map); static stack with the real limits: modem 456 B spare (was about -820 B at 512 words), status SMS 744, main 1016 | Pass |
| v6.9.16 | Built and host-tested only (not on a unit yet): learned radio mode (net_policy test: 10 site scenarios, file formats), voice-only registration wait, no-SIM stop; all three configs build (119,952 / 120,124 / 118,748 B of 131,072); hosted update tests; modem stack 400 B spare | Pass |
| v6.9.17 | Built and host-tested only (not on a unit yet): operator-tied learning and the 2G-to-4G rescue (net_policy test: 2G switched off, SIM swap same/other operator, unreadable IMSI, file round trips); all three configs build (120,440 / 120,612 / 119,236 B); hosted update tests; modem stack 368 B spare | Pass |
| v6.9.15 | Staging v6.9.17 on 1922: "UPDATE STOPPED FLASH ERROR" (update task stack across 0x20080000) | **Fail**, fixed in v6.9.18 |
| v6.9.18 | Built and host-tested only: every flash write from a main-RAM buffer (all four at 0x1000xxxx in the map); IAP status logged on a failed staging; all three configs build (120,732 / 120,900 / 119,528 B); hosted update tests | Pass |
| v6.9.18 | 1922: written to slot A by MCU-Link on the LPC1768 (sectors 17-20 only; journal, settings, records untouched), read back identical; 18:30 slot sent | Pass |
| v6.9.18 | 1922: update from v6.9.18 itself (SAME F/W, REDO): PENDING written to journal page 8 (the write that failed on v6.9.15), 12 swap steps, trial boot, CONFIRMED, "UPDATE OK, now running v6.9.18" | Pass |
| v6.9.18 | BIHAR 1923: written to slot A by MCU-Link from v6.9.16 (journal REPORTED, nothing in progress; settings 001923 untouched), read back identical | Pass |
| v6.9.16 | BIHAR 18:15/18:30, first slots on learned mode: in 4G-only mode its modem reported registration by +CGREG (not +CEREG), so the slot was counted as 2G and 4G as missing (`P2 ... M02`); 18:15 also waited 17 s on +CREG (voice only) before 2G | **Fail** (misclassified), fixed in v6.9.19 |
| v6.9.19 | Built and host-tested only: registered mode taken from the mode the modem was held to (4G-only = 4G whichever of CEREG/CGREG/CREG answers); all three configs build (120,892 / 121,060 / 119,688 B); hosted tests | Pass |
| v6.9.18 | 1922 19:00 slot: 4G in 1 s, data refused once ("no network service"), radio restart and re-attach, sent at +89 s (about 20 s lost to the network); then the scheduled GPS search ran 90 s with no fix: slot 3 min | Network and GPS, not the radio mode logic |
| v6.9.20 | Built and host-tested only (not on a unit yet): automatic GPS off; data recovery stays on the SIM's operator (fresh attach on the same APN, then only that operator's APNs; the full list only for an unknown SIM: worst case under a minute instead of 3); 4G "registration denied" (+CEREG: 0,3) switches to 2G at once; sleep waits for a new firmware's confirmation (at most 2 min); all three configs build (121,400 / 121,568 / 120,204 B); hosted tests | Pass |
| v6.9.19 | BIHAR: installed from SD by v6.9.18 (18:51), so its update path works; 18:45-19:15 registered on LTE in about 1 s, `RATMODE.DAT` back to `P4 ... R01` | Pass |
| v6.9.20 | Installed from SD on both units (1922 from v6.9.18, BIHAR from v6.9.19), "UPDATE OK, now running v6.9.20" reported by the user | Pass |
| v6.9.21 | Built and host-tested only: application compiled with `-ffunction-sections -fdata-sections` (the link already had `--gc-sections`), so unused functions are dropped (USB-stick host, `f_mkfs`, `f_printf`, NXP driver helpers): 121,400 to 108,596 B (BIHAR), about 22 KB free of 128 KB; vector table identical, stack figures identical; hosted tests | Pass |
| v6.9.21 on units (2026-11-08) | 1922: SD install 19:36, then SAME F/W REDO 20:41 (staged in 8 s, confirmed at 30 s). BIHAR: SD install 20:48 (staged in 11 s). Slots on both 6-8 s, stacks and heap (6,944 B) as on v6.9.20, no new errors. **SIM swap BSNL to Airtel on both:** operator change logged (40471 to 40445), learning reset, `airteliot.com` chosen, one 16 s search allowance then 6 s; first Airtel slot on BIHAR had two harmless `CGACT` "unknown" replies (the modem attached on power-on with the old `bsnlnet` context, which Airtel accepted). **Card out on 1922 20:42-22:28:** every slot sent; the seven records written to the card from flash at 22:31 in order, no gaps or doubles; the swap made while the card was out was logged at the first slot with the card back | Pass |
| Overnight 2026-11-08/09, both units back on BSNL SIMs | 1922 (its own SIM 404719180719647): all 33 slots 23:30-07:30 sent in 6 s, no "no network service" at all, so the evening refusals were the network, not the SIM. BIHAR (its own SIM 404719200808782): all slots sent, 7-9 s, except 23:45 (72 s: first slot after the swap, data up on the old `airteliot.com`, `NETOPEN` failed twice, sent by modem HTTP) and 07:00 (37 s: `CIPOPEN` by name got no answer in 20 s, the retry by IP got `+CIPOPEN: 0,4 ERROR` at once as socket 0 was still opening, sent by modem HTTP). Swap back logged on both (40445 to 40471) | Pass; two fixes in v6.9.22 |
| v6.9.22 | Built and host-tested only: after activating data, `AT+CGCONTRDP` is read and if the connection is on another of this firmware's APNs (left from another SIM), a fresh attach on the SIM's own APN before sending; an APN in the carrier's own words counts as fine (parser host-tested on the logged replies, suffix `.mncNNN.mccNNN.gprs`, quotes, case, bad replies). A `CIPOPEN` with no answer at all now closes the network (`AT+NETCLOSE`) so the next try starts clean. All three configs build (108,852 / 108,828 / 107,728 B); hosted update tests; static stack unchanged (modem 368 B spare) | Pass |
| v6.9.22 on units (2026-11-09) | SD install on both (BIHAR 07:41, 1922 the same morning). BIHAR SIM swaps: to Airtel at 08:00 (data already on `airteliot.com`, no re-attach needed, 10 s); back to BSNL at 08:15: data came up on `airteliot.com` again, the check saw it, re-attached on `bsnlnet` and sent in **21 s** (the same case took 72 s on v6.9.21). Stacks and heap unchanged | Pass |
| v6.9.12 + 845-v1.5 | BIHAR 1923 off 19:00 to 09:00, both installed by SD at power-up | Pass: 19:15-08:30 sent as -110 with CUM 10, 08:45 as -112 with CUM 0, live from 09:00, backlog done in 4 slots (15 a slot); 845 UPDATE OK |

### Not tested yet

- Bad-clock wait (v6.9.4 change)
- GPS job stack margin, and GPS fix at all (no fix seen, even with the antenna)
- Damaged-frame count comparison (no damaged frames occurred)
- KSNDMC_TWS on a unit and server
- `rtc_lost845.py safety` (safety wake alone) on a unit
- v6.9.13 slot naming on a unit (needs the LPC845 clock lost at a slot)
- Real battery drain; long outages; full card; power cuts during settings save and firmware swap on the real unit
- Crash capture with the stack in AHB SRAM (needs a forced hard fault in a test build)

---

## 11. Known issues and open items

| Item | Status |
|---|---|
| GPS never gets a fix | Deferred (user, 2026-11-08). Every search is a cold start of 90 s. **Automatic searches off from v6.9.20** (`GPS_AUTO_SEARCH 0`): they added about 93 s to the 01:00/07:00/13:00/19:00 slots (19:00 on 1922 took 3 min). The GPS menu still searches |
| Blank LCD seen once | Closed: the 12 V battery was not connected (user, 2026-11-08) |
| BSNL 4G at 1922 since 17:45, 2026-11-08 | Registration denied once (+CEREG: 0,3), then 4G registers but refuses data ("no network service") until a radio restart; recovered each time in about 20 s. Network side. v6.9.20: denial goes straight to 2G; recovery re-attaches on bsnlnet instead of trying Airtel/Jio APNs. Both units moved to Airtel SIMs on 2026-11-08 evening: no refusals since. Whether it follows 1922's BSNL SIM is untested |
| Sleep during a new firmware's first 30 s | An unconfirmed trial boot counts against it (3 and the old firmware returns). v6.9.20 holds sleep until confirmed, at most 2 min (1922 18:50: power save pressed in the same second as the confirmation) |
| `CIPOPEN` timeouts | Seen once (BIHAR 07:00, 2026-11-09): no answer in 20 s, and the retry failed at once because socket 0 was still opening. v6.9.22 closes the network after an unanswered open so the retry by IP gets a clean start. Watch |
| First slot after a SIM swap | The modem attaches on power-on with the APN it last stored, before the firmware sets the new one. Airtel accepted `bsnlnet` and sent; BSNL gave an address on `airteliot.com` but `NETOPEN` failed (BIHAR 23:45, 72 s). v6.9.22 checks the APN in use and attaches again on the SIM's own APN first (about 10-20 s, once per swap) |
| Status SMS marked "Flag Invalid" by the server | Ignored (user, 2026-11-08) |
| RAM | Fixed in v6.9.11: heap in AHB SRAM, about 16.8 KB main RAM free (was 764 B). Confirm on the long run |
| SMS on 2G can take over a minute | Watch (power) |
| Bihar SMS number | Temporary 9980945474; the ESP32 uses 9741391102 |
| No low-battery handling | Open: a flat battery tries the modem every slot and can reset in a loop (1922 on 2026-11-08). Skip the modem below a set voltage and keep the records |
| Log noise | Fixed in v6.9.14: the SMS poll and socket closes are no longer logged unless a message arrives |
| Modem task stack | **Fixed in v6.9.15.** v6.9.10 cut it from 768 to 512 words, but `bench/stack_callgraph.py` kept the old limits, so the "200 B spare" reported since was against 768 words: at 512 the worst path (an SMS command reply that fails, restarts a silent modem and syncs the clock) was about 820 B short. 74 words free was measured on 1922 in the safety-wake slot (2026-11-08 16:07). Now 832 words (456 B spare in the worst case), heap 20 KB; the script's limits match the firmware |
| LPC845 clock set back across a boundary | A server correction of 90 s or more across xx:00/15/30/45 runs that boundary twice: the stored 15-min sample keeps only the second part (CUM_RF stays right). Rare; not fixed |
| LPC845 I2C0 jams while the modem is on | Recovered by 845-v1.5. Hardware cause (noise or supply dip on the RTC bus) worth a look on the next board |
| RTC coin cell | Can't be measured on this board; firmware can only detect a stop. ADC link proposed for the next board |

---

## 12. Plan

**Done:** stack, watchdog, error log, LPC845 protection, rain count kept, RAM relieved (v6.9.11); RTC jam recovery
(845-v1.5); gap fill for the day after an off period (v6.9.12); KSNDMC_TRG on a second unit.

**Batch 1 status (v6.9.14 + 845-v1.6, 2026-11-08):** modem lockup (rail held off between tries), `$f7` lost
(re-sent), data sanity (logged), log noise, RTC watch on stale reads: done; on 1922 since 2026-11-08 16:00.
SD lockup: already covered (every card wait has a time limit; a hang is caught by the 60 s watchdog, card loss
tested in v6.9.4). Dead LPC845 link: already logged as ERROR and the slot filled later; no change. Bad-clock wait:
passed on 1922 (`rtc_lost845.py safety`). Low-battery handling: on hold.

**Batch 2 code review (v6.9.15, 2026-11-08):** every write that a power cut can interrupt, read before any power
is pulled. Found and fixed: (1) `FLASHSYN.DAT` was saved once after a catch-up of up to 96 records, so a cut part way
through wrote every record already copied to the card again (only the newest repeat is recognised); it is now saved
after each record. (2) The settings sector (station ID, unit type, calibration, GPS position) has one copy, erased and
rewritten on each save, with no spare sector for a second; a cut in between left it blank and the unit came back as
the build default ID (`001061` for a BIHAR build), sending under the wrong station without a word. A blank sector now
takes the ID from the newest stored record's payload and logs ERROR; only a unit with no records uses the default.
The GPS position save runs during an unattended slot with the modem on, the likeliest time for a weak-battery
brownout. Checked and safe: state files (fixed length, overwritten in place), day files (newest row repeat
recognised), backlog file (entries appear only once their size is written at close; a cut between a send and
marking it sent sends it once more), ERRORS.TXT rotation, the record log and error log in flash (CRC on every page,
torn pages skipped, a half-erased sector erased again), the LPC845 rain total after a battery disconnect (returned
from the newest record; only rain while unpowered and in the unfinished slot is missing). Still to do on the unit:
the power-cut tests themselves.

**Next batches**, each tested before the next:

1. Handling failures: modem and SD lockups, `$f7` ignored, data sanity (out-of-range values, rain during a bad clock),
   dead LPC845 link report and fill, bad-clock test.
2. Power cycling: power cuts during every kind of write, card loss, full card.
3. Power and network flow: SIM check first (a no-SIM slot to about 20-30 s), recovery by failure type (CFUN, rescan,
   stop early on denied), roaming (clear blocked networks), 4G every slot: today it starts on the mode that last worked (4G by default) and
   switches after 5 polls, so a unit that once fell back to 2G stayed there. **Done in v6.9.16 (learned per site,
   `net_policy.c`, host test `LPC1768/tests/net_policy_test.c`):** 4G is searched first for about twice its learned
   registration time (6-20 s; 16 s with no history). Where 2G is what delivers, 4G gets a 6 s look, or none once 4G
   has been missing 4 times or registered without data twice; a full 20 s 4G check runs every 16 power-ons, spaced to
   32 and 64 while those checks fail, and back to 16 once 4G works. A slot that registers on 4G but whose data fails
   twice running moves the unit to 2G first; 2G failing the same way moves it back. An outage on both teaches
   nothing. CREG alone (voice/SMS) waits up to 8 s more for data registration (CEREG/CGREG). The mode registered on is the mode the modem
   was held to (v6.9.19): BIHAR's modem reports LTE registration by CGREG. A SIM reported "not
   inserted" 3 times stops the power-on at once (was about 30 s). State in `RATMODE.DAT`, 30 fixed characters, e.g.
   `P4 C00 R01 M00 F00 G00 X0 L0 N40471`; older files are read. **v6.9.17:** the learning belongs to one operator
   (IMSI MCC+MNC, the `N` field): a SIM from another operator starts afresh, another SIM from the same operator
   keeps it; and with 4G skipped, no 2G after 15 s switches to 4G in the same power-on (2G switched off, or a 4G-only
   SIM such as Jio), so a unit can't sit on a mode that has gone until the next full check. Simulated per day: good 4G 1.0 s of 4G search per slot,
   2G-only 0.8 s, 4G-without-data 0.1 s with 4 first sends delayed a slot, then adaptive wake (measure first).

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
