#!/bin/bash
# Builds every image from the current source and packages one release for the test team.
#
#   RELEASE/make_release.sh
#
# Versions are read from the source (LPC1768/user_config.h, LPC845/boot/fw845_app.h,
# TEST_JIG/migrate/main.c). Write Antigravity/RELEASE/LPC/<main version>/RELEASE_NOTES.md first: the script
# stops without it, so no bundle goes out without its notes. It then:
#   1. builds the CTRL (LPC845) firmware and the old-unit migration image
#   2. builds the main firmware for KSNDMC_TRG, BIHAR_TRG and KSNDMC_TWS (copies of the tree, so
#      user_config.h in the repo is never edited)
#   3. runs the host tests against the new images
#   4. copies the files into the common release folder, Antigravity/RELEASE/LPC/<version>/ (23 Nov 2026),, writes SHA256SUMS.txt, and zips the email bundle:
#      SD_CARD/ and RELEASE_NOTES.pdf only
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REL="${ROOT}/RELEASE"                       # this script and notes_to_pdf.py
# 23 Nov 2026: releases go to the common release folder next to the projects (Antigravity/RELEASE/LPC),
# not into this repo. RELEASE_DIR overrides it.
DEST="${RELEASE_DIR:-$(cd "${ROOT}/.." && pwd)/RELEASE}/LPC"
mkdir -p "${DEST}"

MAIN_VER=$(awk '$1=="#define" && $2=="FIRMWARE_VERSION" {gsub(/"/,"",$3); print $3}' "${ROOT}/LPC1768/user_config.h")
CTRL_VER=$(awk '$1=="#define" && $2=="FW845_VERSION" {gsub(/"/,"",$3); print $3}' "${ROOT}/LPC845/boot/fw845_app.h")
MIG_VER=$(awk '$1=="#define" && $2=="MIG_VERSION" {gsub(/"/,"",$3); print $3}' "${ROOT}/TEST_JIG/migrate/main.c")
PROD=$(awk '$1=="#define" && $2=="PRODUCTION_BUILD" {print $3}' "${ROOT}/LPC1768/user_config.h")
# v7.01: FIRMWARE_VERSION is the bare number ("7.01"); release folders and names keep the "v" (v6.9.66, v7.01)
MAIN_VER="v${MAIN_VER#v}"
OUT="${DEST}/${MAIN_VER}"
NAME="LPC-AIO_${MAIN_VER}_CTRL_${CTRL_VER}_${MIG_VER}"

if [ "${PROD}" != "1" ]; then
    echo "[ERROR] PRODUCTION_BUILD is not 1 in LPC1768/user_config.h: a bench build can't be released"; exit 1
fi
echo "=== Release ${NAME} (production build; debug log only on cards with DEBUGLOG.ON) ==="
if [ ! -f "${OUT}/RELEASE_NOTES.md" ]; then
    echo "[ERROR] Write ${OUT}/RELEASE_NOTES.md first"; exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/aio_release.XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
LOGS="${WORK}/logs"; mkdir -p "${LOGS}"

echo "--- 1. CTRL ${CTRL_VER} and migration ${MIG_VER}"
(cd "${ROOT}/LPC845" && ./build.sh > "${LOGS}/ctrl.log" 2>&1) || { tail -20 "${LOGS}/ctrl.log"; exit 1; }
(cd "${ROOT}/TEST_JIG/migrate" && ./build.sh > "${LOGS}/mig.log" 2>&1) || { tail -20 "${LOGS}/mig.log"; exit 1; }

echo "--- 2. Main firmware ${MAIN_VER}, three unit types"
for cfg in KSNDMC_TRG BIHAR_TRG KSNDMC_TWS; do
    T="${WORK}/tree_${cfg}"; mkdir -p "${T}"
    rsync -a --exclude '*.o' --exclude 'Debug/*.hex' "${ROOT}/LPC1768" "${ROOT}/LPC845" "${ROOT}/scripts" "${ROOT}/FreeRTOS_Library" "${T}/"
    ln -s "${ROOT}/tools" "${T}/tools"
    ln -s "${ROOT}/CMSISv1p30_LPC17xx" "${T}/CMSISv1p30_LPC17xx"
    sed -i '' "s/^#define UNIT_CFG .*/#define UNIT_CFG        ${cfg}/" "${T}/LPC1768/user_config.h"
    (cd "${T}/LPC1768" && ./build.sh > "${LOGS}/${cfg}.log" 2>&1) || { tail -20 "${LOGS}/${cfg}.log"; exit 1; }
    grep -E "^Image" "${LOGS}/${cfg}.log" | sed "s/^/    ${cfg}: /"
done

echo "--- 3. Host tests"
OUT="${WORK}/ht" KSN_TRG_HEX="${WORK}/tree_KSNDMC_TRG/LPC1768/Debug/fw_ksn_trg.hex" \
    KSN_TWS_HEX="${WORK}/tree_KSNDMC_TWS/LPC1768/Debug/fw_ksn_tws.hex" \
    bash "${ROOT}/bench/host_tests.sh" > "${LOGS}/host_tests.log" 2>&1 || true
OUT="${DEST}/${MAIN_VER}"
if grep -qvE "ALL PASS" "${LOGS}/host_tests.log"; then
    cat "${LOGS}/host_tests.log"; echo "[ERROR] Host tests did not all pass: no release"; exit 1
fi
echo "    $(grep -c "ALL PASS" "${LOGS}/host_tests.log") test sets, ALL PASS"

echo "--- 4. Package"
rm -rf "${OUT}/SD_CARD" "${OUT}/FACTORY_MCU-LINK" "${OUT}/OTA"
mkdir -p "${OUT}/SD_CARD" "${OUT}/FACTORY_MCU-LINK" "${OUT}/OTA"
cp "${WORK}/tree_KSNDMC_TRG/LPC1768/Debug/fw_ksn_trg.hex" "${OUT}/SD_CARD/"
cp "${WORK}/tree_BIHAR_TRG/LPC1768/Debug/fw_bihar.hex" "${OUT}/SD_CARD/"
cp "${WORK}/tree_KSNDMC_TWS/LPC1768/Debug/fw_ksn_tws.hex" "${OUT}/SD_CARD/"
cp "${ROOT}/LPC845/ctrl.hex" "${OUT}/SD_CARD/"
cp "${ROOT}/TEST_JIG/migrate/out/fw.hex" "${OUT}/SD_CARD/"
# 24 Nov 2026: OTA/ (files to upload to the office page): the three main files and ctrl.hex, the same files as on
# the card (before, this folder was made by hand)
cp "${OUT}/SD_CARD/fw_ksn_trg.hex" "${OUT}/SD_CARD/fw_bihar.hex" "${OUT}/SD_CARD/fw_ksn_tws.hex" "${OUT}/SD_CARD/ctrl.hex" "${OUT}/OTA/"
cp "${WORK}/tree_KSNDMC_TRG/LPC1768/Debug/LPC1768_AIO_FULL.hex" "${OUT}/FACTORY_MCU-LINK/MAIN_KSNDMC_TRG_${MAIN_VER}_FULL.hex"
cp "${WORK}/tree_BIHAR_TRG/LPC1768/Debug/LPC1768_AIO_FULL.hex" "${OUT}/FACTORY_MCU-LINK/MAIN_BIHAR_TRG_${MAIN_VER}_FULL.hex"
cp "${WORK}/tree_KSNDMC_TWS/LPC1768/Debug/LPC1768_AIO_FULL.hex" "${OUT}/FACTORY_MCU-LINK/MAIN_KSNDMC_TWS_${MAIN_VER}_FULL.hex"
cp "${ROOT}/LPC845/LPC845_TWS.hex" "${OUT}/FACTORY_MCU-LINK/CTRL_${CTRL_VER}_FULL.hex"
python3 -I "${REL}/notes_to_pdf.py" "${OUT}/RELEASE_NOTES.md" | sed "s/^/    /"

(cd "${OUT}" && find OTA SD_CARD FACTORY_MCU-LINK -type f ! -name .DS_Store | sort | xargs shasum -a 256 > SHA256SUMS.txt)
GIT_DESC="$(cd "${ROOT}" && git rev-parse --short HEAD)$(cd "${ROOT}" && git diff --quiet HEAD -- '*.c' '*.h' '*.s' '*.ld' '*.sh' || echo ' + uncommitted source changes')"
echo "Built $(date '+%Y-%m-%d %H:%M') from ${GIT_DESC}" > "${OUT}/BUILD_INFO.txt"

# The email bundle: only the SD card files and the release notes (user, 2026-11-13). The MCU-Link
# images, checksums and build info stay in Antigravity/RELEASE/LPC/<version>/ for us.
rm -f "${DEST}/${NAME}.zip"
(cd "${OUT}" && zip -qr "${DEST}/${NAME}.zip" SD_CARD RELEASE_NOTES.pdf -x "*.DS_Store")
echo "=== Done: ${DEST}/${NAME}.zip ($(du -h "${DEST}/${NAME}.zip" | cut -f1)) ==="
