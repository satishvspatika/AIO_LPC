# Bench scripts

Scripts used on the bench unit (BIHAR_TRG, station 1923) to watch, probe and fault-test both chips. They need
`pyocd` (`~/Library/Python/3.9/bin/pyocd`) and an MCU-Link. Symbol addresses are read from the current `.axf` with
`tools/arm-toolchain/bin/arm-none-eabi-nm` (the repo's `tools` links to the TWS6 toolchain), so they follow each build, except where noted.

Plugging the MCU-Link into J2 resets the LPC845. The LPC1768 is only powered at slots or when an operator wakes it,
so LPC1768 scripts work only while it is on. See `docs/SYSTEM_DESIGN.md`, section "Bench methods".

## Watching

| Script | Chip | What it does |
|---|---|---|
| `mon845.py DUR STEP` | LPC845 | Prints journal state, reset reason, wake reason, rail bits and the last received frame every STEP seconds for DUR seconds; prints only on change |
| `mon1768.py` | LPC1768 | Same idea for the LPC1768 while it is on |
| `state845.py`, `state1768.py` | both | One snapshot of the main variables |
| `read845.py [slotB.bin]` | LPC845 | Frames, receive state, journal, and checks slot A (and slot B against an image if given) |
| `rails845.py` | LPC845 | Rail bits: `111` all off, `100` slot, `000` all on |
| `uart845.py` | LPC845 | Link receive and transmit buffers |
| `snap845.sh` | LPC845 | Three snapshots including PC, LR and SP: for a hung LPC845 |
| `card_read.sh [HH:MM]` | SD (Mac) | `ERRORS.TXT`, boots, restarts, slot ends and stack lines from the day's log, last slots and record, then ejects. `COPY=file` also copies an update file |

## Fault tests

| Script | What it does |
|---|---|
| `fault845.py` | Forces an LPC845 crash, a damaged stack edge and a hung main loop, and times each restart (expected 0.1 s, 0.1 s, 60 s) |
| `raincount_keep845.py` | Checks the rain count kept in `.noinit` survives an LPC845 restart |
| `force_tips0_845.py` | Sets the day's tips to 0 to test the never-dip rule on `CUM_RF` |
| `reset845_from1768.py` | Holds the LPC845 in reset from the LPC1768 and watches the link drop and come back. **Its addresses are fixed for a v6.9.x map: update them first** |
| `slotwake845.py` | Moves the LPC845 to just before a slot, to start one on demand |
| `wdt_rate845.py` | Measures the LPC845 watchdog clock (2205 ticks/s) |

LPC1768 faults (stack overflow, stuck UI task, all tasks stuck) are forced from the keypad in a `TEST_FAULTS` build:
KEY TEST, then UP, DOWN or RIGHT.

## Build and checks

| Script | What it does |
|---|---|
| `host_tests.sh` | Builds and runs the bootloader tests for both chips on the Mac with sanitizers. Output goes to `$OUT` (default `/tmp/aio_bench`) |
| `stack_static.sh [UNIT_CFG]` | Worst-case stack per function from the compiler's call graph (`stack_callgraph.py`) |
| `flash845.sh` | Flashes `LPC845/LPC845_TWS.hex` by SWD, retrying |
| `swd_probe_check.sh` | Tries SWD at several speeds and modes when the probe won't connect |
