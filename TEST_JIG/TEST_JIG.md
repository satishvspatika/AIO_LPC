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
