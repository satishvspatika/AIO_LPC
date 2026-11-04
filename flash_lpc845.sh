#!/bin/bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LINKSERVER="/tmp/linkserver/LinkServer"
CONFIG="${ROOT}/scripts/lpc845-mculink.json"
HEX="${ROOT}/LPC845/LPC845_TWS.hex"
AXF="${ROOT}/LPC845/LPC845_TWS.axf"
if [ ! -f "${HEX}" ] && [ -f "${ROOT}/LPC845/build_v2/LPC845_TWS.hex" ]; then
    HEX="${ROOT}/LPC845/build_v2/LPC845_TWS.hex"
    AXF="${ROOT}/LPC845/build_v2/LPC845_TWS.axf"
fi

echo "=== Flashing LPC845 Coprocessor Firmware ==="
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

echo "=== LPC845 Flashed, Verified, and Running! ==="
