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
    echo "  ~/Library/Python/3.9/bin/pyocd pack install LPC1768"
    exit 127
fi

CONFIG="${ROOT}/scripts/lpc1768-mculink.json"
HEX="${ROOT}/LPC1768/Debug/LPC1768_AIO_FULL.hex"
AXF="${ROOT}/LPC1768/Debug/LPC1768_AIO.axf"

echo "=== Flashing LPC1768 (Unified AIO Full Firmware) ==="
echo "Flasher tool: ${FLASHER}"

if [ "$FLASHER" = "LINKSERVER" ]; then
    pkill -9 -f redlinkserv 2>/dev/null || true
    pkill -9 -f LinkServer 2>/dev/null || true

    # Sector 1 (0x1000 - 0x1FFF) stores non-volatile station parameters (station ID, URLs, keys).
    if [ "$1" == "--mass-erase" ] || [ "$1" == "--erase-all" ]; then
        echo "1. Mass Erasing LPC1768 (512 KB) [EXPLICITLY REQUESTED]..."
        "${LINKSERVER}" flash -p '#1' -u none "${CONFIG}" erase
        pkill -9 -f redlinkserv 2>/dev/null || true
    else
        echo "1. Preserving Flash Configuration (Sector 1 Station ID & Parameters)..."
    fi

    echo "2. Programming Boot Stub (0x0) + Application (0x18000)..."
    "${LINKSERVER}" flash -p '#1' -u none "${CONFIG}" load "${HEX}" 2>&1 | tee /tmp/lpc1768_flash.log || true
    if grep -q "Writing Flash ended with an error" /tmp/lpc1768_flash.log || grep -q "Hardware Transfer Error" /tmp/lpc1768_flash.log || grep -q "Could not connect to core" /tmp/lpc1768_flash.log; then
        echo "   [ERROR] Flash writing encountered an SWD hardware transfer error!"
        echo "   Please check physical ribbon cable connection and re-run."
        exit 1
    fi
    if grep -q "Finished writing Flash successfully" /tmp/lpc1768_flash.log; then
        echo "   Flash write confirmed OK (Finished writing Flash successfully)."
    else
        echo "   [ERROR] Flash write did not complete!"
        exit 1
    fi
    pkill -9 -f redlinkserv 2>/dev/null || true

    echo "3. Issuing Hardware Reset Pulse..."
    "${LINKSERVER}" probe '#1' wiretimedreset 50 || true
    pkill -9 -f redlinkserv 2>/dev/null || true

elif [ "$FLASHER" = "PYOCD" ]; then
    ERASE_MODE="sector"
    if [ "$1" == "--mass-erase" ] || [ "$1" == "--erase-all" ]; then
        echo "1. Mass Erasing LPC1768 [EXPLICITLY REQUESTED]..."
        ERASE_MODE="chip"
    else
        echo "1. Preserving Flash Configuration (Sector 1 Station ID & Parameters)..."
    fi

    echo "2. Programming Boot Stub (0x0) + Application (0x18000) via PyOCD..."
    ${PYOCD} flash -t lpc1768 --erase "${ERASE_MODE}" "${HEX}"

    echo "3. Issuing Hardware Reset..."
    ${PYOCD} reset -t lpc1768
fi

echo "=== LPC1768 Flashed and Running! ==="
