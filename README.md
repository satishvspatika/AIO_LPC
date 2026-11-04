# Unified AIO Telemetry Station (LPC1768 & LPC845)

Unified firmware codebase for the All-In-One (AIO) Telemetry Weather Station:
- **LPC1768** (ARM Cortex-M3): Main telemetry application, FreeRTOS scheduler, SIM7600 4G LTE modem driver, FAT/SD card data logging, USB host, power management, inter-MCU protocol.
- **LPC845** (ARM Cortex-M0+): Sensor acquisition and ultra-low power coprocessor (rain gauge, wind sensors, ADC, RTC, comm protocol).

---

## 1. Prerequisites (Setup on Any Laptop)

### A. ARM GCC Toolchain
The build scripts automatically detect any of the following:
1. Standard system `arm-none-eabi-gcc` in your `PATH`
2. NXP MCUXpresso IDE installation (under `/Applications/MCUXpressoIDE*`)
3. Local `tools/arm-toolchain/bin` folder

**Recommended setup on macOS:**
```bash
brew install --cask gcc-arm-embedded
```
Verify the installation:
```bash
arm-none-eabi-gcc --version
```

### B. Python 3
Required for vector checksum patching (`scripts/lpc_checksum.py`):
```bash
python3 --version
```

### C. (Optional) Flashing Tools
- NXP LinkServer / MCU-Link debug probe
- Used by `flash_lpc1768.sh` and `flash_lpc845.sh`

---

## 2. Building Firmware

To compile both targets:
```bash
./build_all.sh
```

Or build individually:
```bash
# Build LPC1768
./LPC1768/build.sh

# Build LPC845
./LPC845/build.sh
```

**Build Outputs:**
- **LPC1768**: `LPC1768/Debug/LPC1768_AIO_FULL.hex` (Sector 0 Boot Stub + Sector 16 Application merged)
- **LPC845**: `LPC845/LPC845_TWS.hex`

---

## 3. Flashing to Hardware

Connect your MCU-Link debug probe via SWD ribbon cable to the target header:

```bash
# Flash LPC1768 (preserves Sector 1 Station ID & Calibration config)
./flash_lpc1768.sh

# Flash LPC845 coprocessor
./flash_lpc845.sh
```

---

## 4. Git Synchronization & Versioning

### Pushing Updates (with Auto-Versioning & Tags)
Whenever you push changes, the script automatically increments the version tag (e.g. `v6.0.1` $\rightarrow$ `v6.0.2`), or you can supply an explicit tag with `-t`. The commit message (`-m`) is always optional:

```bash
# 1. Automatic version bump (e.g. v6.0.1 -> v6.0.2)
./push_git.sh
# Or with a custom description:
./push_git.sh -m "Fixed sensor ADC sampling"

# 2. Specify a custom tag explicitly (overrides auto-bump)
./push_git.sh -t "v6.1.0"
# Or with custom description:
./push_git.sh -t "v6.1.0" -m "Major release with new telemetry protocol"
```

The script will:
1. Update `FIRMWARE_VERSION` in `LPC1768/user_config.h` (visible on 16x2 LCD Line 2 as `ID:001061 v6.X.Y`).
2. Recompile all firmware targets via `./build_all.sh`.
3. Commit all changes, create the Git tag, and push everything to GitHub.

### Pulling Updates on Another Laptop
To pull the latest code or checkout a specific release tag on another machine:
```bash
# Pull latest main and automatically build
./get_git.sh

# Or checkout a specific tag
./get_git.sh -tag v6.0.1

# Then flash to board
./flash_lpc1768.sh
```

---

## 5. Repository Structure

```
AIO_LPC/
├── build_all.sh               # Master build script
├── flash_lpc1768.sh           # LinkServer flashing script for LPC1768
├── flash_lpc845.sh            # LinkServer flashing script for LPC845
├── LPC1768/                   # Cortex-M3 firmware source and build script
│   ├── build.sh
│   ├── main.c
│   ├── sim7600_driver.c/h
│   ├── storage_log.c/h
│   ├── intermcu_protocol.c/h
│   └── Debug/                 # Linker scripts and target hex files
├── LPC845/                    # Cortex-M0+ firmware source and build script
│   ├── build.sh
│   ├── main.c
│   └── build_v2/              # Driver objects & target hex files
├── FreeRTOS_Library/          # FreeRTOS kernel & static library
├── CMSISv1p30_LPC17xx/        # CMSIS core & peripheral drivers
└── scripts/                   # Boot stub assembly & checksum patcher
```
