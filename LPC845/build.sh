#!/bin/bash
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPTS_DIR="${DIR}/../scripts"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Auto-detect ARM GCC toolchain and MCUXpresso IDE
MCUX_BIN=$(ls -d /Applications/MCUXpressoIDE*/ide/plugins/com.nxp.mcuxpresso.tools.*/tools/bin 2>/dev/null | tail -n 1 || true)
if [ -z "$MCUX_BIN" ]; then
    MCUX_BIN=$(ls -d /Applications/MCUXpresso\ IDE*/MCUXpresso\ IDE.app/Contents/Eclipse/plugins/com.nxp.mcuxpresso.tools.*/tools/bin 2>/dev/null | tail -n 1 || true)
fi

if [ -n "$MCUX_BIN" ] && [ -x "${MCUX_BIN}/arm-none-eabi-gcc" ]; then
    TC_DIR="${MCUX_BIN}"
    CC="${TC_DIR}/arm-none-eabi-gcc"
    OBJCOPY="${TC_DIR}/arm-none-eabi-objcopy"
    SIZE="${TC_DIR}/arm-none-eabi-size"
    MCUX_TOOLS=$(cd "${MCUX_BIN}/.." && pwd)
elif [ -x "${PROJECT_ROOT}/tools/arm-toolchain/bin/arm-none-eabi-gcc" ]; then
    TC_DIR="${PROJECT_ROOT}/tools/arm-toolchain/bin"
    CC="${TC_DIR}/arm-none-eabi-gcc"
    OBJCOPY="${TC_DIR}/arm-none-eabi-objcopy"
    SIZE="${TC_DIR}/arm-none-eabi-size"
elif command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    CC="arm-none-eabi-gcc"
    OBJCOPY="arm-none-eabi-objcopy"
    SIZE="arm-none-eabi-size"
else
    echo "[ERROR] arm-none-eabi-gcc toolchain not found!"
    echo "Please install MCUXpresso IDE or GNU Arm Embedded Toolchain, or add it to PATH."
    exit 127
fi

echo "=== 1. Compiling LPC845 main.c ==="
${CC} -c -mcpu=cortex-m0plus -mthumb \
    -DCPU_LPC845 -DCPU_LPC845M301JBD48 -D__USE_CMSIS \
    -I"${DIR}" \
    -I"${DIR}/CMSIS" \
    -I"${DIR}/device" \
    -I"${DIR}/drivers" \
    -I"${DIR}/board" \
    -I"${DIR}/utilities" \
    -O2 -g -Wall -fcommon -ffunction-sections -fdata-sections \
    "${DIR}/main.c" -o "${DIR}/main.o"

# Always link with the toolchain's GCC C library, so every machine builds the same image. The
# prebuilt driver objects in build_v2 were compiled for Redlib, so drivers are compiled from source.
echo "=== 2. Compiling drivers & linking LPC845_TWS.axf (GCC libraries) ==="
GCC_OBJ_DIR="${DIR}/build_gcc"
mkdir -p "${GCC_OBJ_DIR}"
GCC_OBJS=""
for SRC in board/clock_config board/pin_mux device/system_LPC845 \
           drivers/fsl_adc drivers/fsl_clock drivers/fsl_common drivers/fsl_gpio drivers/fsl_i2c \
           drivers/fsl_power drivers/fsl_reset drivers/fsl_sctimer drivers/fsl_swm drivers/fsl_syscon drivers/fsl_usart; do
    OBJ="${GCC_OBJ_DIR}/$(basename "${SRC}").o"
    ${CC} -c -mcpu=cortex-m0plus -mthumb \
        -DCPU_LPC845 -DCPU_LPC845M301JBD48 -D__USE_CMSIS -DNDEBUG \
        -I"${DIR}" -I"${DIR}/CMSIS" -I"${DIR}/device" -I"${DIR}/drivers" -I"${DIR}/board" -I"${DIR}/utilities" \
        -O2 -g -fcommon -ffunction-sections -fdata-sections \
        "${DIR}/${SRC}.c" -o "${OBJ}"
    GCC_OBJS="${GCC_OBJS} ${OBJ}"
done

${CC} -mcpu=cortex-m0plus -mthumb -nostartfiles --specs=nano.specs --specs=nosys.specs \
  -Wl,--gc-sections -Wl,--defsym=__main=main -Wl,-Map="${DIR}/LPC845_TWS.map" \
  -L"${DIR}/Debug" \
  -T "${DIR}/Debug/lpcxpresso845max_gpio_led_output_Debug.ld" \
  -o "${DIR}/LPC845_TWS.axf" \
  "${DIR}/main.o" "${DIR}/build_v2/startup/startup_lpc845.o" ${GCC_OBJS}

echo "=== 3. Generating Binary & Patching Checksum ==="
${OBJCOPY} -O binary "${DIR}/LPC845_TWS.axf" "${DIR}/LPC845_TWS.bin"
python3 "${SCRIPTS_DIR}/lpc_checksum.py" "${DIR}/LPC845_TWS.bin"
${OBJCOPY} -I binary -O ihex "${DIR}/LPC845_TWS.bin" "${DIR}/LPC845_TWS.hex"
${SIZE} "${DIR}/LPC845_TWS.axf"
echo "=== LPC845 Build Completed (GCC libraries)! ==="
echo "=== LPC845 Build Successful! Output: ${DIR}/LPC845_TWS.hex ==="
