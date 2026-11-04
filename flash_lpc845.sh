#!/bin/bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Auto-detect LinkServer or PyOCD flasher
FLASHER=""
LINKSERVER=""
PYOCD=""

# Check for LinkServer
if command -v LinkServer >/dev/null 2>&1; then
    LINKSERVER="LinkServer"
    FLASHER="LINKSERVER"
elif [ -x "/tmp/linkserver/LinkServer" ]; then
    LINKSERVER="/tmp/linkserver/LinkServer"
    FLASHER="LINKSERVER"
elif [ -x "/usr/local/bin/LinkServer" ]; then
    LINKSERVER="/usr/local/bin/LinkServer"
    FLASHER="LINKSERVER"
else
    MCUX_LS=$(ls -d /Applications/MCUXpresso*/**/LinkServer 2>/dev/null | head -n 1 || true)
    if [ -n "$MCUX_LS" ] && [ -x "$MCUX_LS" ]; then
        LINKSERVER="$MCUX_LS"
        FLASHER="LINKSERVER"
    fi
fi

# Fallback: Check for PyOCD (CMSIS-DAP)
if [ -z "$FLASHER" ]; then
    if command -v pyocd >/dev/null 2>&1; then
        PYOCD="pyocd"
        FLASHER="PYOCD"
    elif [ -x "${HOME}/Library/Python/3.9/bin/pyocd" ]; then
        PYOCD="${HOME}/Library/Python/3.9/bin/pyocd"
        FLASHER="PYOCD"
    elif python3 -m pyocd --version >/dev/null 2>&1; then
        PYOCD="python3 -m pyocd"
        FLASHER="PYOCD"
    fi
fi

if [ -z "$FLASHER" ]; then
    echo "[ERROR] Neither NXP LinkServer nor PyOCD flasher was found!"
    echo "To install PyOCD for direct MCU-Link flashing, run:"
    echo "  python3 -m pip install --user pyocd"
    echo "  ~/Library/Python/3.9/bin/pyocd pack install LPC845"
    exit 127
fi

CONFIG="${ROOT}/scripts/lpc845-mculink.json"
HEX="${ROOT}/LPC845/LPC845_TWS.hex"
AXF="${ROOT}/LPC845/LPC845_TWS.axf"
if [ ! -f "${HEX}" ] && [ -f "${ROOT}/LPC845/build_v2/LPC845_TWS.hex" ]; then
    HEX="${ROOT}/LPC845/build_v2/LPC845_TWS.hex"
    AXF="${ROOT}/LPC845/build_v2/LPC845_TWS.axf"
fi

echo "=== Flashing LPC845 Coprocessor Firmware ==="
echo "Flasher tool: ${FLASHER}"

if [ "$FLASHER" = "LINKSERVER" ]; then
    pkill -9 -f redlinkserv 2>/dev/null || true
    pkill -9 -f LinkServer 2>/dev/null || true

    echo "1. Mass Erasing LPC845 (64 KB)..."
    "${LINKSERVER}" flash -p '#1' -u none "${CONFIG}" erase
    pkill -9 -f redlinkserv 2>/dev/null || true

    echo "2. Programming Firmware (37 KB)..."
    "${LINKSERVER}" flash -p '#1' -u none "${CONFIG}" load "${HEX}"
    pkill -9 -f redlinkserv 2>/dev/null || true

    echo "3. Verifying Firmware in Flash..."
    "${LINKSERVER}" flash -p '#1' -u none "${CONFIG}" verify "${AXF}"
    pkill -9 -f redlinkserv 2>/dev/null || true

    echo "4. Issuing Hardware Reset Pulse..."
    "${LINKSERVER}" probe '#1' wiretimedreset 50 || true
    pkill -9 -f redlinkserv 2>/dev/null || true

elif [ "$FLASHER" = "PYOCD" ]; then
    echo "1. Programming LPC845 firmware via PyOCD..."
    ${PYOCD} flash -t lpc845 --erase chip "${HEX}"

    echo "2. Issuing Hardware Reset..."
    ${PYOCD} reset -t lpc845
fi

echo "=== LPC845 Flashed, Verified, and Running! ==="
