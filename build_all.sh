#!/bin/bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=========================================================="
echo "  Building Unified AIO Telemetry Station (LPC1768 & LPC845)"
echo "=========================================================="
"${ROOT}/LPC1768/build.sh"
echo ""
"${ROOT}/LPC845/build.sh"
echo ""
echo "=========================================================="
echo "  BUILD ALL SUCCESSFUL!"
echo "  - LPC1768: ${ROOT}/LPC1768/Debug/LPC1768_AIO_FULL.hex"
echo "  - LPC845:  ${ROOT}/LPC845/LPC845_TWS.hex"
echo "=========================================================="
