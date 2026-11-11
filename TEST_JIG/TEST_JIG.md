# Test Jig: Design, Learnings and Test Record

The programming jig for AIO units: it takes an old unit (any firmware, or none) and leaves it with the
SD-card bootloaders and current firmware on both chips, set up and ready for the field. From then on
the unit updates itself from its own SD card.

This folder is kept apart from the production firmware (`LPC1768/`, `LPC845/`) and its design record
(`docs/SYSTEM_DESIGN.md`). Jig code and jig learnings go here only. Production firmware never contains
jig code, so a field unit can never act as a programmer.

Started 2026-11-10. Replaces the earlier ESP32 jig idea: the jig is one of our own AIO units, whose
SD card, image checks, flash layout, LCD and keypad we already know.

---

## 1. Roles

| Unit | Role | Notes |
|---|---|---|
| Jig | An AIO unit running the jig firmware | Bench: 1922. Goes in and out of jig mode by an ordinary SD update (the jig is just another application image under the same bootloader) |
| Target | The unit being programmed | Bench: BIHAR 1923 (known firmware, card backed up, MCU-Link can always restore it) |

While it is the jig, 1922's modem is unplugged: its connector J8 gives the target-reset line (section 3).

---

## 2. Hardware facts (schematics SIT_DL_1768_V2, SIT_DL_845_V2)

### The board was designed for in-circuit programming over the NXP ROM UART loader

| LPC1768 pin | Net | Reaches |
|---|---|---|
| P0.5 (pin 80) | RST_845 | LPC845 RESET (PIO0_5), through jumper JP1 on the 845 board |
| P1.0 (pin 95) | -PGM_845 | LPC845 PIO0_12 (ROM loader entry pin), through jumper JP2; 4K7 pull-up R4 |
| P4.28 TXD3 (pin 82) | MTx1_SRx0_ISP | LPC845 PIO0_24 and PIO0_0 (both tied): the ROM loader's receive pin on the LPC845, and on the LPC824 |
| P4.29 RXD3 (pin 85) | MRx1_STx0_ISP | LPC845 PIO0_25 and PIO0_4 (both tied): the ROM loader's transmit pin |

UART3 is today's CTRL link, so it is also the LPC845's loader UART. The production firmware does not
use P0.5 or P1.0 (checked 2026-11-10): the pull-ups keep the LPC845's RESET and PGM high, as in normal
running.

### LPC1768 headers (1768 board)

| Header | Pins |
|---|---|
| J4 JTAG_LPC1768 | 1 SWDIO, 2 GND, 3 SWCLK, 4 TRESET. Dedicated debug pins on the LPC1768: they cannot be used as GPIO, so a unit can be programmed through J4 but can never drive it |
| J5 -PGM_LPC1768 (ROM UART loader) | 1 +3V3, 2 -PGM_LPC1768 (P2.10), 3 -RST_LPC1768 (RESET), 4 U_TXD0_ISP (P0.2), 5 U_RXD0_ISP (P0.3), 6 GND |
| J8 GPRS (modem) | 1 CTx2_GRx, 2 CRx2_GTx, 3 GPRS_3V8, 4 GPRS_STATUS (P0.0, 10K pull-up), 5 GPRS_POWER (P0.1), 6 GND |
| J9 Pwr_Ctrl-Lpc_845 | to the 845 board's J4: 845_3V3, -PGM_845, RST_845, the two UART lines, ROW2, COL2, SHDN_3V3/3V8/5V, GND, +12V |
| J2 | USB connector, not a programming header |

UART0 (P0.2/P0.3) is unused in the production firmware (`ispuart.c` is old test code, never called), so
the jig has it free for programming. The 1768 board's crystal is 12 MHz.

### LPC845 headers (845 board)

| Header | Pins |
|---|---|
| J2 JTAG_LPC845 (MCU-Link) | 1 SWDIO (270R, 10K pull-up), 2 GND, 3 SWCLK (270R, 10K pull-down), 4 RESET1 |
| JP1 / JP2 | Jumpers joining RST_845 and -PGM_845 from the LPC1768 to the LPC845's RESET and PIO0_12. Must be fitted for in-circuit programming |

On the LPC845 the SWD and RESET pins can be switched to GPIO, so a jig's CTRL board could drive SWD from
its own J2 if ever needed. Not the chosen route (section 4).

---

## 3. Wiring: jig to target (LPC1768 programming)

Both units on their own 12 V. **3V3 is not connected** between them (it could back-power one board from
the other).

| Jig | Target | Purpose |
|---|---|---|
| J5 pin 4 U_TXD0 (P0.2) | J5 pin 5 U_RXD0 | data to the target |
| J5 pin 5 U_RXD0 (P0.3) | J5 pin 4 U_TXD0 | data from the target |
| J5 pin 2 (P2.10, as GPIO) | J5 pin 2 -PGM_LPC1768 | hold low at reset: ROM loader |
| J8 pin 5 GPRS_POWER (P0.1) | J5 pin 3 -RST_LPC1768 | target reset |
| J5 pin 6 GND | J5 pin 6 GND | common ground |

The jig's own J5 pin 3 is the jig's own RESET: never connected. Between operations the jig leaves P0.1
and P2.10 as inputs, so the target runs normally while the cable is on.

The target's LPC1768 only has power while the target's CTRL board keeps its rails on: press DSP on the
target before programming (about a minute awake), or later a plug that holds the rails on.

---

## 4. How a unit is programmed

1. **Target LPC1768, from the jig, over J5** (ROM UART loader):
   read part ID, boot code version and serial number; erase the whole chip; write boot stub, bootloader,
   application and blank update journal; read back and compare; release reset.
2. **Target LPC845, from the target's own LPC1768**, over the CTRL link (ROM UART loader): hold -PGM_845
   low, pulse RST_845, erase the whole chip, write CTRL bootloader and application, read back, release.
   No cable. Electrically proven possible (section 6, 2026-11-10). Where this code lives is open
   (section 8).
3. Station ID and unit type from the keypad; self-check; LCD shows READY FOR FIELD.
4. One row per unit in `JIG_LOG.CSV` on the jig's card: time, target chip IDs and serials, versions
   written, station, type, result.

Why the ROM UART loader and not SWD: it needs no SWD driver, works whatever firmware is on the chip
(unless code read protection disables it), and the boards already have the wiring. The protocol is the
one FlashMagic and lpc21isp use (NXP UM10360 for the LPC1768, UM11029 for the LPC845); details to be
taken from those manuals when the code is written, not from memory.

---

## 5. Software layout (planned)

- `TEST_JIG/` holds the jig's own sources and `build.sh`. It may compile shared drivers from `LPC1768/`
  (SD card, FatFs, LCD, keypad, FreeRTOS) read-only; nothing jig-specific is added to `LPC1768/` or
  `LPC845/`.
- The jig image is built for the same flash layout as the production application (slot A), with the
  same image header, so the production bootloader installs it from SD and can install production
  firmware back over it.
- Files on the jig's card: the target images (`LPC1768_AIO_FULL.hex` parts, `LPC845_TWS.hex`), checked
  with the same rules as `scripts/fw_check_hex.py` before use.

---

## 6. Test record

| Date | Test | Result |
|---|---|---|
| 2026-11-10 07:46 | **Rails with the LPC845 held in reset** (1922, probe on the 845 board's J2). The ROM loader leaves the LPC845's pins at their reset state, so the LPC1768 must stay powered while the LPC845 is in reset. Rails switched on by probe, LPC1768 booted 07:46:22; LPC845 held in reset 10 s (07:46:55-07:47:05). The LPC1768 log runs on without a gap (uptime +28 s to +77 s, modem answering every 2 s), no new boot; the LPC845 restarted with all rails on | Pass |
| 2026-11-11 14:54 | **Old unit LPC1768 read** (original 1768 + 845 boards, old firmware, no SD card; MCU-Link on J4, attach, read only; `TEST_JIG/old_read.py`). Dump `TEST_JIG/old_units/old_2114_lpc1768.bin` (git-ignored: sector 1 holds server login), SHA1 4af70a31. **CRP word 0x2FC = 0xFFFFFFFF: no read protection** (SWD and the ROM loader both open). Layout as expected: sector 0 = jump loader (about 0.4 s delay loop, VTOR = 0xB000, jump to 0xB000; same as `../Bootloader_Jump/Bootloader_Jump/bootloader/src/main.c`); 0x1000 settings (station 002114, KSNDMC, TRG, app "TRG5-1.0.5"); 0x2000-0x38A3 day data (records dated 27-28 May 2025); 0xB000-0x16FFF old SD bootloader (FatFs, `fw.hex`); 0x18000-0x38D5A application; 0x39000-0x7FFFF blank. The 2017/2019 builds on disk do not match byte for byte (other builds), but the code paths are the same. The new bootloader's area (0x2000-0x8FFF) holds only old day data, not the old bootloader, so the migration can write it while the old bootloader stays intact | Pass: SD route possible for this unit's LPC1768 |
| 2026-11-11 15:02 | **Old unit LPC845 read** (MCU-Link on the 845 board's J2, attach, read only; plugging in reset it: SYSRSTSTAT 3). CPUID 0x410CC601, DEVICE_ID 0x8454 (LPC845, 64 KB). Dump `TEST_JIG/old_units/old_2114_lpc845.bin` (git-ignored), SHA1 2f2507c1. Old CTRL app at 0x0-0x7797 (30 KB, sectors 0-29 of 1 KB), no bootloader, rest blank. **Word at 0x2FC = 0x009B3306: code, not a CRP value** (CRP1/2/3 and NO_ISP are 0x12345678, 0x87654321, 0x43218765, 0x4E697370), so no protection: the ROM loader and PIO0_12 entry are open. Live pins: RESET function enabled on PIO0_5 (PINENABLE0 0xFF7FCD9F); PIO0_12 input, high; UART0 TX PIO0_25, RX PIO0_24 (PINASSIGN0 0xFFFF1819) = the ROM loader's pins; rails PIO0_26/27 low (modem, LPC1768 on), PIO0_28 high (LCD off) | Pass: SD + JP1/JP2 route possible for this unit's LPC845 |
| 2026-11-11 15:05 | **Old unit power-up with JP1 + JP2 fitted** (caps fitted with the unit off, MCU-Link left on J2, then 12 V on). LPC845 running the old CTRL app (PC 0x1968 in flash, not the ROM loader), PIO0_5 and PIO0_12 high, rails 26/27/28 all on. SYSRSTSTAT 0x3 (power-on + reset pin): either a short dip on RESET while the LPC1768 powered up, or the probe; harmless here. To separate: one power-up with probe on and caps off | Pass: field order "caps on, then power on" works |
| 2026-11-11 15:21 | **Survey MIG-S0.1 on old unit 2114**, JP1 + JP2 fitted, 1922's card. Ran, then the unit went dark at the LPC845 step: the LPC845 sat in its ROM loader (PC 0x0F000736), rail lines inputs, LPC1768 unpowered (no SWD answer). Rerun with JP1/JP2 off: SURVEY.TXT complete (image check OK, OLD JUMP LOADER, old bootloader present, CRP none, settings text), OLDFLASH.BIN equal to the probe dump except day data 0x2000-0x2C03 (old firmware kept recording) | Fail at the LPC845 step: **old unit's power board switches the LPC1768 off while the LPC845's rail lines float** (1922's did not, 2026-11-10) |
| 2026-11-11 15:27-15:32 | **Rail hold-up on 2114** (probe pulses on the LPC845's RESET; old CTRL drives the rail lines again within a few ms of reset, its Pin_init runs first): 3 s = LPC1768 restarts; 20 ms = steady; 100 ms = LCD flickers, LPC1768 keeps running; 300 ms = backlight off briefly, LPC1768 keeps running | LPC1768 survives at least 300 ms of floating rail lines |
| 2026-11-11 15:40-16:05 | **Survey S0.2-S0.8 on 2114, JP1 + JP2 fitted** (probe on the 845's J2 to read its RAM after each run). S0.2-S0.5: unit dark after the stub was written. Probe: stub byte-perfect in RAM; run by hand to the IAP call it drives the rails (LPC1768 powers up) and IAP 57 keeps them held. Stopwatch in the W data: synced at 5-11 ms, W accepted at 14 ms. Hold-up stream (S0.5, one word per ms into a long W): **the LPC1768 stops at 58 ms** once the ROM loader runs (its pull-ups switch the rails off; in plain reset the pins are off and the rails last over 300 ms). S0.6/S0.7: survives, but after the data the ROM sends no "OK", and "G" gets "0" + 0x8D (CR cut short). S0.7 also: stub cut to 52 bytes, LPC1768 at 25 MHz during the window. **S0.8: no waiting for "OK" or for G's answer; the stub is proven by synchronising again.** Report: step 6, J = 0 / 33876 (0x8454, LPC845 64 KB), K = 0 / 1 / 13, N = 0 / 201543722 / 2937783520 / 1505891545 / 4110424325; "after W: (nothing)", "after G: 30 8D" (the stub restarts the loader mid-reply); LPC845 restarted with its firmware | **Pass: LPC845 ROM loader reached and held over the CTRL link on an old unit** |
| 2026-11-11 16:25 | **M1 (MIG-M1.0) on old unit 2114**, SD + JP1/JP2 only (probe left on the LPC1768's J4, so not involved). LCD: CTRL NOW NEW / REMOVE JP1+JP2. Then probe on the 845's J2: **whole 64 KB equal to the factory files** (CTRL bootloader at 0x0, v1.10 at 0x2400, rest blank: 0 bytes differ), new CTRL running (VTOR 0x2400), rails driven. Old CTRL flash saved by M1 to MIGRATE/OLD845.BIN | **Pass: LPC845 converted to the new CTRL by SD card + 2 jumpers** |

---

## 7. Milestones

| # | Milestone | Writes to target | Status |
|---|---|---|---|
| 1 | Jig reads target LPC1768 part ID, boot version, serial over J5 | No | Next |
| 2 | Jig reads target LPC1768 flash and compares with the image file | No | |
| 3 | Full program of target LPC1768 (BIHAR) + read-back; BIHAR boots and runs slots | Yes | |
| 4 | Target LPC1768 programs its own LPC845 + read-back | Yes | |
| 5 | Keypad set-up, READY FOR FIELD, JIG_LOG.CSV | | |
| 6 | First old field unit | Yes | |

Recovery at every step: MCU-Link restores both chips of either unit.

---

## 8. Open items

1. JP1 and JP2 fitted on BIHAR's 845 board and on the old units (needed for step 2).
2. Code read protection on old units: read one old unit's flash with MCU-Link + pyOCD (J4, then J2). A
   level that disables the loader pin would need another route.
3. Where step 2 lives: in production firmware as a menu item (also recovers a field unit whose CTRL
   bootloader is damaged; about 3-4 KB of 21 KB free) or only in a jig-supplied image. To decide.
4. Old units with unknown CTRL firmware: the target's LPC1768 is powered only while that firmware keeps
   the rails on. Likely a plug holding the rails on during programming.
5. Programming time per unit (estimate about a minute for both chips), to measure.
6. Field conversion order and the dedicated jig hardware: sections 10 and 11.

---

## 9. Route B: no jig for old units that still have the old bootloader (2026-11-10)

Old units carry an SD bootloader of their own (`../Bootloader_Jump/Bootloader_Main1`, "Bootloader V 1.1").
It is **not** the same as the new one:

| | Old bootloader V1.1 | New bootloader (LPC1768/boot) |
|---|---|---|
| Location | 0xB000-0x17FFF (sectors 11-16), entered from a jump at 0x0 | 0x2000-0x8FFF (sectors 2-8), boot stub at 0x0 |
| File | `fw.hex` in the card's root; deleted after a good burn | `fw_<type>.hex`, chosen from the menu |
| Method | Erases sectors 17-22, writes the hex straight into 0x18000-0x3FFFF, jumps to 0x18000 | Stages in slot B, journal, CRC check, swap, trial run, roll back |
| Checks | Per-line checksum only; a bad line is skipped (later bytes shift), no image check. On a failed burn it stops (`while(1)`) and `fw.hex` stays, so a power cycle retries | Whole-image CRC, refuses protected areas, rollback |
| Hex format | Records appended in order into 4 KB blocks (ignores record addresses inside a block); only type 02 segment records | Any |

Old flash use (TWS5 `memory.c`): day data in sectors 2-8 (0x2000-0x8FFF), which is where the new bootloader
goes; old bootloader in sectors 11-16, which is the new error log, journal and swap. So the new production
firmware cannot simply be loaded as `fw.hex`: it would find old data where it expects its journal.

Our build's hex is compatible with the old parser: `fw_bihar.hex` is 16-byte type 00 records from 0x18000,
type 02 segment records only, no gaps (checked 2026-11-10).

**Plan: a one-time migration image**, loaded by the old bootloader as `fw.hex`, kept in `TEST_JIG/` (not
production):
1. Check its own CRC before touching anything (the old bootloader does not); if bad, stop and say so:
   the old bootloader is still intact, so copying `fw.hex` again and power-cycling retries.
2. Program the LPC845 through its ROM loader over the CTRL link (CTRL bootloader + app from the card), read back.
3. Write the new bootloader into sectors 2-8, read back.
4. Rewrite sector 0 with the new boot stub. **The only unsafe moment** (about 0.1 s): a power cut here
   needs the MCU-Link. Before this the old path boots, after it the new one.
5. Erase sector 1 (settings), 9-16 (old bootloader: new error log, journal, swap) and 25-29 (record log);
   write the blank journal. The new bootloader must treat old bytes in the journal as no record, for a cut
   between steps 4 and 5 (to prove in the host tests).
6. Stage the production application through the normal update path and restart: the new bootloader
   installs it; the operator sets station ID and type.

**JP1/JP2 (decided 2026-11-10):** on 1922 they are 2-pin headers with no caps (open). Fit caps only for the
migration and remove them after: with them on, an unpowered (sleeping) LPC1768 might pull the LPC845's RESET
or PGM line low, which would hold the CTRL board in reset or start it in the ROM loader after a restart.
Not yet measured with caps on (baseline 08:06 with caps off, LPC1768 asleep: PIO0_5 = 1, PIO0_12 = 1).
The migration image senses them (LPC1768 pull-downs on P0.5 and P1.0: fitted reads high through the
845 board's 4K7 pull-ups R1/R4, open reads low), shows FIT JP1 + JP2 before programming the LPC845 and
REMOVE JP1 + JP2 at the end, and does not finish until they are off.

Still needs a jig or MCU-Link: blank new boards, units whose old bootloader is damaged, units without
JP1/JP2 (no LPC845 ROM loader from the LPC1768).

Before building: read one real old unit with the MCU-Link (LPC1768 0x0-0x7FFFF, LPC845 0x0-0xFFFF) to
confirm this layout, the jump at 0x0 and the bootloader version; check JP1/JP2. Bench test: write that
old image onto BIHAR with the MCU-Link, then migrate it from its card.

---

## 10. Field conversion plan (decided 2026-11-11)

Goal: convert old field units to the new firmware (new bootloaders on both chips). No MCU-Link in the
field. Order of routes, easiest first:

| # | Route | Needs on site | For |
|---|---|---|---|
| 1 | Migration image (section 9) | SD card + JP1/JP2 caps | Old units with a working old bootloader V1.1 |
| 2 | Jig over J5 (sections 3-4) | Jig unit + cable + JP1/JP2 caps | Old bootloader damaged or missing, code read protection, blank boards |
| 3 | Board swap | Pre-programmed 1768 and 845 boards | No JP1/JP2 headers, or anything stuck; old boards back to the lab |

Route 1 in the field: fit JP1 + JP2, insert the card (`fw.hex` = migration image, plus the new images),
power-cycle; the LCD ends with REMOVE JP1 + JP2, then station ID and type from the keypad. The
migration holds the LPC845 in reset early, so the old CTRL firmware cannot switch the LPC1768 off
mid-migration (rails stay on with the LPC845 in reset: Pass 2026-11-10, section 6).

**Old vs new LPC1768 bootloader: not the same, not the same place.** Old: jump at 0x0, bootloader
0xB000-0x17FFF, file `fw.hex`. New: boot stub at 0x0, bootloader 0x2000-0x8FFF, `fw_<type>.hex`.
Only the application start (0x18000) is shared. Hence the migration image.

**LPC845 from SD card + 2 jumpers: yes, if** (to check on one old unit in the lab first):
1. JP1/JP2 headers exist on old 845 boards and can be reached on site (a 2026-11-06 note says field
   units' JP1/JP2 can't be reached: confirm what that means, e.g. sealed enclosure).
2. The old LPC845 has no code read protection level that disables the ROM loader.
3. Old 845 boards match SIT_DL_845_V2 (UART3 to PIO0_24/25, JP1 to RESET, JP2 to PIO0_12).
4. Milestone 4 (LPC1768 programs its own LPC845) passes on the bench.

The LPC845's ROM loader is in ROM and cannot be erased, so the old CTRL firmware does not matter.

---

## 11. Jig hardware (2026-11-11)

**Cable.** Each 1768 board has the same J5, but a straight J5-J5 cable does not work: TX/RX must cross,
the jig's own J5 pin 3 is the jig's RESET, and 3V3 must not be joined. The jig end is J5 plus one wire
from J8 pin 5 (P0.1); the target end is J5 (table in section 3).

**A dedicated jig needs no GPRS board and no 845 board:**

| Part | Jig | Target |
|---|---|---|
| GPRS board | Out (J8 pin 5 is the target-reset line) | Not needed for programming |
| 845 board | Can come out, with the SHDN plug below | Must stay: it is programmed in step 2 |

What the 845 board does on the jig, and the replacement:
1. **Power.** The 1768 board's 3V3, 5V and 3V8 come from the power board (J1), switched by SHDN_3V3,
   SHDN_5V, SHDN_3V8 from the LPC845 (active low, 0 = on). Without the 845 board they float. **SHDN
   plug** on the jig's J9: pin 8 SHDN_3V3 and pin 10 SHDN_5V (LCD) to pin 11 GND; pin 9 SHDN_3V8 open
   (modem rail off). The jig is then always on (no DSP press, no sleep). To confirm with a meter on the
   power board first (no schematic for it).
2. **Clock.** The DS1338 RTC is on the 845 board, so a jig without it has no real time: number the
   `JIG_LOG.CSV` rows, or set the time from the keypad at start-up.
3. **Keypad.** No loss: COL2's pull-up R19 is on the 1768 board (845_3V3 comes from the power board via
   J10).

While 1922 is the development jig it keeps its 845 board, so it can go back to field duty.
(2026-11-11: BIHAR 1923 is with the testing team; 1922 stays the bench/jig unit.)

---

## 12. Old unit facts for the SD + 2 jumper migration (gathered 2026-11-11)

Sources: the old unit's LPC1768 read (section 6), live pin reads on it, `../Bootloader_Jump/` sources,
the old CTRL sources `../DMCTRG845/`, `../BIHARTRG845/` (same code), `../TWS845/`, and the old app
source `../TWS5_1_0_5_recovered_Jan2026/` (the unit runs TRG5-1.0.5, whose source we do not have).

### Old LPC1768 flash (unit 2114)

| Range | Contents | New layout use |
|---|---|---|
| 0x0000-0x0FFF | Jump loader: ~0.4 s delay, VTOR = 0xB000, jump | New boot stub (the one unsafe rewrite) |
| 0x1000-0x1FFF | Old settings (station, type, server) | New settings |
| 0x2000-0x38A3 (sectors 2-8 region) | Old day data | New bootloader: **free to overwrite** |
| 0xB000-0x16FFF | Old SD bootloader V1.1 | New error log, journal, swap (erase last) |
| 0x18000-0x38D5A | Old application TRG5-1.0.5 | Slot A (migration image runs here) |
| 0x39000-0x7FFFF | Blank | Slot B, record log |

CRP word = 0xFFFFFFFF: no protection.

### Old SD bootloader V1.1: rules the migration image's `fw.hex` must follow

From `Bootloader_Main1/Example01/main.c`:
1. Runs at every power-up: ~0.4 s jump loader + ~0.4 s delay, mounts the card, opens **`fw.hex` in the
   root** (8.3 name). No file: jumps to 0x18000.
2. Erases **sectors 17-22 (0x18000-0x47FFF)** before reading the file, then writes 4 KB blocks from
   0x18000. Loop stops at 0x40000: **image must be under 160 KB (0x28000)**, else it ends without the
   end record and hangs (`while(1)`) with the app already erased (recoverable: fix `fw.hex`, power-cycle).
3. Hex parsing: records are packed in order into each 4 KB block, the block address taken from the last
   record's address + type 02 segment. So the hex must be **contiguous, 16-byte type 00 records, type 02
   segment records only (no type 04), no gaps**, lines under 100 characters, and must **end with
   `:00000001FF`** (without it `f_gets` returns NULL forever: hang). Our builds meet this (checked
   2026-11-10 for `fw_bihar.hex`); `fw_check_hex.py` should check it for the migration image too.
4. A record with a bad checksum is skipped silently (later bytes shift): the migration image must check
   its own CRC before doing anything.
5. On success it **deletes `fw.hex`** and jumps to 0x18000 (VTOR set). On a power cut mid-burn `fw.hex`
   stays, so the next power-up burns again.
6. Card: SD/SDHC (CMD8, CCS handled), FatFs with FAT12/16/32. **No exFAT**: use cards formatted FAT32
   (32 GB or less).
7. It never touches the LPC845 and never sends a CTRL command.

### Old LPC1768 application (live, unit 2114)

P0.5 (RST_845) and P1.0 (-PGM_845): **inputs with pull-ups, both high** (FIODIR 0, PINMODE 0). The old
firmware never drives them (also not in the TWS5 source). So fitting JP1/JP2 does not disturb the old
LPC1768 firmware while it runs.

### Old CTRL firmware (DMCTRG845 / BIHARTRG845)

1. **At power-up it switches all rails on** (LPC1768, modem, LCD: `main.c:1070`).
2. Rails go off only on the LPC1768's `f` (sleep) command or after **32 minutes** without one
   (`main.c:1153`); every 15 minutes it switches the LPC1768 and modem on.
3. So the migration image gets power at power-up without a DSP press and keeps it for up to 32 minutes,
   provided it never sends `f`. After it holds the LPC845 in reset, the rails stay on regardless (Pass
   2026-11-10).
4. CTRL link: LPC845 UART0 at 9600 baud. The ROM loader uses the same pins (PIO0_24 RX, PIO0_25 TX).

### Field procedure (first draft)

1. Power off. Fit JP1 and JP2 on the 845 board.
2. Insert a FAT32 card with `fw.hex` (migration image) and the new images (`fw_<type>.hex`,
   `ctrl.hex` and the CTRL bootloader), copied from the old card if one is in the unit: keep its data.
3. Power on. Old CTRL switches the LPC1768 on; old bootloader burns `fw.hex` (about 1 minute) and
   deletes it; the migration image starts and shows progress on the LCD.
4. LCD: REMOVE JP1 + JP2. Station ID, type and server are carried over from the old settings sector.

### Still to find out

1. ~~Old LPC845: CRP, layout~~ Done 2026-11-11: no CRP, app at 0x0 (30 KB), ROM loader pins as expected (section 6). No old CTRL build on disk to compare with (sources only).
2. Old 845 board: JP1/JP2 headers present (user, 2026-11-11). Board revision: to note.
3. Decided 2026-11-11: carry station ID, type and server over from the old settings sector automatically (old layout at 0x1000 to be decoded from the dump).
4. With JP1/JP2 fitted and the LPC1768 unpowered (old CTRL asleep), does the LPC845's RESET get pulled
   low through the unpowered LPC1768? Matters only if a unit sits with the caps on while asleep; the
   procedure fits them with the unit off and removes them at the end. To measure once.

---

## 13. Migration image (`TEST_JIG/migrate/`, started 2026-11-11)

Built by `TEST_JIG/migrate/build.sh` into `TEST_JIG/migrate/out/fw.hex` (git-ignored build output).
Bare metal (no RTOS), linked for 0x18000, carries the same image info block as production images
(profile 0xFFFFFFFF, so it claims no station type). Compiles the production SD/FAT, NXP drivers and
`LPC1768/boot/boot_core.c` + `fw_journal.c` read-only. `oldboot_check_hex.py` checks every `fw.hex`
against the old bootloader's parser (and simulates it).

How it installs production firmware on the LPC1768: it writes the new boot stub, bootloader and blank
journal itself, stages the production app in slot B, appends FW_J_PENDING and restarts. The new
bootloader then does the normal swap and trial (the same path as every SD update). The migration image
ends up in slot B as the "previous" image, so it must be safe to run again on a converted unit.

Build note: GCC deletes code after a read from address 0 unless `-fno-delete-null-pointer-checks`
(first build came out at 2.8 KB without it).

### Stage S (survey), MIG-S0.1: writes nothing to either chip

Its flash functions refuse (stubs). It checks its own CRC, reports sector 0 and the old bootloader,
copies 0x0-0x17FFF to `MIGRATE/OLDFLASH.BIN`, senses JP1/JP2, and with both fitted enters the LPC845
ROM loader (115200, then 57600, then 9600), reads J, K, N raw, and restarts the LPC845. Report:
`MIGRATE/SURVEY.TXT`. LCD ends with SURVEY OK / REMOVE JP1+JP2, or says what failed.

Note: the old bootloader erases the old application to load any `fw.hex`. To put unit 2114's old
firmware back afterwards: `TEST_JIG/old_units/old_2114_restore_fw.hex` (from the dump, checked
compatible) as `fw.hex`, power-cycle. Without an application, the old CTRL still powers the LPC1768
every 15 minutes, which reruns the survey (harmless: same copy, LPC845 restarted).

### Stage M (migration): to write after the survey passes

LPC845 ROM loader writes (erase, write RAM, copy to flash, read back; CTRL bootloader at 0x0 and app
at 0x2400), then LPC1768: settings carried over from the old sector 1 format, new bootloader (sectors
2-8), blank journal, production app staged in slot B, boot stub last, PENDING, restart.

### Stage S2 (MIG-S0.2): hold the rails with a RAM stub on the LPC845

The rail-line problem above, solved in software: enter the ROM loader at 115200 in one go (sync,
`A 0`, `U 23130`, `W 268437504 100` + the stub, `G 268437504 T`: about 30 ms of traffic, against at least
300 ms of hold-up). The stub (`stub845.s`, 100 bytes at 0x10000800, above the loader's 2 KB) turns on the
GPIO and SWM clocks, unassigns SPI0 MOSI/MISO/SSEL (the ROM's interface auto-detect may put SPI0 on
PIO0_26/27), drives PIO0_27 (LPC1768) and PIO0_28 (LCD) low and PIO0_26 (modem) high, then calls IAP
57 (Reinvoke ISP, param 1 USART). The LPC1768 then resynchronises without hurry. Leaving: a 2 ms reset
pulse; the old CTRL drives the lines again within a few ms. Facts from NXP UM11029 rev 1.7 (sections
5.3.7.2, 5.4, 5.5.1-5.5.8, 5.6.8); the report records the last step reached and the ms the lines floated.

### Survey result (MIG-S0.8, 2026-11-11): what the migration must do at the LPC845 step

1. LPC1768 at 25 MHz (CCLKCFG 15) for the window; UART3 115200; PGM low, 2 ms reset, PGM released at
   the first "Synchronized" (about 5 ms).
2. Sync, `A 0`, `U 23130`, `W 268437504 52` + the 52-byte stub: no "OK" follows the data; do not wait.
3. `G 268437504 T`: the reply is cut ("0" + 0x8D) because the stub restarts the loader at once. Do not
   wait for it; back to 100 MHz; synchronise again (only a restarted loader answers).
4. Budget: the LPC1768 lasts about 58 ms after reset once the ROM runs; the stub starts at about 22 ms.
5. Leaving: 2 ms reset with PGM released; the old CTRL drives the rails again within a few ms.

### Stage M1 (MIG-M1.0): LPC845 to the new CTRL, passed 2026-11-11

`ctrl845.c`, CTRL bootloader and firmware embedded from `LPC845/LPC845_BOOT.bin` and `LPC845_TWS.bin`
(build.sh checks their vector checksum, image info, CRC and the CRP word). Order, so a power cut leaves a
unit that comes back by itself: read all 64 sectors (saved to `MIGRATE/OLD845.BIN` while they still hold
the old firmware; nothing written if already the new CTRL); sector 0 <- rescue program (`rescue845.s`:
vector table, drives the LPC1768/LCD rail lines, IAP 57 into the ROM loader, so a power-up after a cut
powers the LPC1768 and the migration carries on); sectors 1-63 erased, written, read back; sector 0 <- the
CTRL bootloader's head last; whole chip read back. Unsafe only between erasing and writing sector 0
(twice, about 20 ms each). Handover at 25 MHz with a 300 ms wait.
