#!/bin/bash
set -e
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LPC_DIR="${PROJECT_ROOT}/LPC1768"
SCRIPTS_DIR="${PROJECT_ROOT}/scripts"
OUT_DIR="${LPC_DIR}/Debug"
mkdir -p "${OUT_DIR}"

# Auto-detect ARM GCC toolchain and MCUXpresso IDE
MCUX_BIN=$(ls -d /Applications/MCUXpressoIDE*/ide/plugins/com.nxp.mcuxpresso.tools.*/tools/bin 2>/dev/null | tail -n 1 || true)
if [ -z "$MCUX_BIN" ]; then
    MCUX_BIN=$(ls -d /Applications/MCUXpresso\ IDE*/MCUXpresso\ IDE.app/Contents/Eclipse/plugins/com.nxp.mcuxpresso.tools.*/tools/bin 2>/dev/null | tail -n 1 || true)
fi

if [ -n "$MCUX_BIN" ] && [ -x "${MCUX_BIN}/arm-none-eabi-gcc" ]; then
    TC_DIR="${MCUX_BIN}"
    CC="${TC_DIR}/arm-none-eabi-gcc"
    AS="${TC_DIR}/arm-none-eabi-as"
    LD="${TC_DIR}/arm-none-eabi-ld"
    OBJCOPY="${TC_DIR}/arm-none-eabi-objcopy"
    SIZE="${TC_DIR}/arm-none-eabi-size"
    MCUX_TOOLS=$(cd "${MCUX_BIN}/.." && pwd)
elif [ -x "${PROJECT_ROOT}/tools/arm-toolchain/bin/arm-none-eabi-gcc" ]; then
    TC_DIR="${PROJECT_ROOT}/tools/arm-toolchain/bin"
    CC="${TC_DIR}/arm-none-eabi-gcc"
    AS="${TC_DIR}/arm-none-eabi-as"
    LD="${TC_DIR}/arm-none-eabi-ld"
    OBJCOPY="${TC_DIR}/arm-none-eabi-objcopy"
    SIZE="${TC_DIR}/arm-none-eabi-size"
elif command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    CC="arm-none-eabi-gcc"
    AS="arm-none-eabi-as"
    LD="arm-none-eabi-ld"
    OBJCOPY="arm-none-eabi-objcopy"
    SIZE="arm-none-eabi-size"
else
    echo "[ERROR] arm-none-eabi-gcc toolchain not found!"
    exit 127
fi

echo "=== 1. Compiling Clean AIO LPC1768 Sources ==="
CFLAGS="-c -mcpu=cortex-m3 -mthumb -O2 -g -fcommon \
  -DDEBUG -D__USE_CMSIS=CMSISv1p30_LPC17xx \
  -I${LPC_DIR} \
  -I${PROJECT_ROOT}/CMSISv1p30_LPC17xx/inc \
  -I${PROJECT_ROOT}/FreeRTOS_Library/include \
  -I${PROJECT_ROOT}/FreeRTOS_Library/portable \
  -I${PROJECT_ROOT}/FreeRTOS_Library/demo_code \
  -I${LPC_DIR}/NXP/Drivers/include \
  -I${LPC_DIR}/fat_sd \
  -I${LPC_DIR}/USBHostLite"

# Compile modular sources
${CC} ${CFLAGS} "${LPC_DIR}/main.c" -o "${OUT_DIR}/main.o"
${CC} ${CFLAGS} "${LPC_DIR}/hal_power.c" -o "${OUT_DIR}/hal_power.o"
${CC} ${CFLAGS} "${LPC_DIR}/intermcu_protocol.c" -o "${OUT_DIR}/intermcu_protocol.o"
${CC} ${CFLAGS} "${LPC_DIR}/telemetry_formatter.c" -o "${OUT_DIR}/telemetry_formatter.o"
${CC} ${CFLAGS} "${LPC_DIR}/sim7600_driver.c" -o "${OUT_DIR}/sim7600_driver.o"
${CC} ${CFLAGS} "${LPC_DIR}/ui_display.c" -o "${OUT_DIR}/ui_display.o"
${CC} ${CFLAGS} "${LPC_DIR}/storage_log.c" -o "${OUT_DIR}/storage_log.o"
${CC} ${CFLAGS} "${LPC_DIR}/flash_nvram.c" -o "${OUT_DIR}/flash_nvram.o"
${CC} ${CFLAGS} "${LPC_DIR}/flash_log.c" -o "${OUT_DIR}/flash_log.o"
${CC} ${CFLAGS} "${LPC_DIR}/cr_startup_lpc17.c" -o "${OUT_DIR}/cr_startup_lpc17.o"

# Compile FatFs drivers
mkdir -p "${OUT_DIR}/fat_sd/option"
${CC} ${CFLAGS} "${LPC_DIR}/fat_sd/diskio.c" -o "${OUT_DIR}/fat_sd/diskio.o"
${CC} ${CFLAGS} "${LPC_DIR}/fat_sd/spi_sd_lpc17xx.c" -o "${OUT_DIR}/fat_sd/spi_sd_lpc17xx.o"
${CC} ${CFLAGS} "${LPC_DIR}/fat_sd/ff.c" -o "${OUT_DIR}/fat_sd/ff.o"
${CC} ${CFLAGS} "${LPC_DIR}/fat_sd/fattime.c" -o "${OUT_DIR}/fat_sd/fattime.o"
${CC} ${CFLAGS} "${LPC_DIR}/fat_sd/option/ccsbcs.c" -o "${OUT_DIR}/fat_sd/option/ccsbcs.o"

# Compile NXP Peripheral Drivers
mkdir -p "${OUT_DIR}/NXP/Drivers/source"
${CC} ${CFLAGS} "${LPC_DIR}/NXP/Drivers/source/lpc17xx_adc.c" -o "${OUT_DIR}/NXP/Drivers/source/lpc17xx_adc.o"
${CC} ${CFLAGS} "${LPC_DIR}/NXP/Drivers/source/lpc17xx_clkpwr.c" -o "${OUT_DIR}/NXP/Drivers/source/lpc17xx_clkpwr.o"
${CC} ${CFLAGS} "${LPC_DIR}/NXP/Drivers/source/lpc17xx_gpdma.c" -o "${OUT_DIR}/NXP/Drivers/source/lpc17xx_gpdma.o"
${CC} ${CFLAGS} "${LPC_DIR}/NXP/Drivers/source/lpc17xx_gpio.c" -o "${OUT_DIR}/NXP/Drivers/source/lpc17xx_gpio.o"
${CC} ${CFLAGS} "${LPC_DIR}/NXP/Drivers/source/lpc17xx_i2c.c" -o "${OUT_DIR}/NXP/Drivers/source/lpc17xx_i2c.o"
${CC} ${CFLAGS} "${LPC_DIR}/NXP/Drivers/source/lpc17xx_nvic.c" -o "${OUT_DIR}/NXP/Drivers/source/lpc17xx_nvic.o"
${CC} ${CFLAGS} "${LPC_DIR}/NXP/Drivers/source/lpc17xx_pinsel.c" -o "${OUT_DIR}/NXP/Drivers/source/lpc17xx_pinsel.o"
${CC} ${CFLAGS} "${LPC_DIR}/NXP/Drivers/source/lpc17xx_ssp.c" -o "${OUT_DIR}/NXP/Drivers/source/lpc17xx_ssp.o"
${CC} ${CFLAGS} "${LPC_DIR}/NXP/Drivers/source/lpc17xx_uart.c" -o "${OUT_DIR}/NXP/Drivers/source/lpc17xx_uart.o"
${CC} ${CFLAGS} "${LPC_DIR}/NXP/Drivers/source/lpc17xx_wdt.c" -o "${OUT_DIR}/NXP/Drivers/source/lpc17xx_wdt.o"
${CC} ${CFLAGS} "${LPC_DIR}/NXP/Drivers/source/lpc17xx_libcfg_default.c" -o "${OUT_DIR}/NXP/Drivers/source/lpc17xx_libcfg_default.o"

# Compile USBHostLite
mkdir -p "${OUT_DIR}/USBHostLite"
${CC} ${CFLAGS} "${LPC_DIR}/USBHostLite/rtc_cal.c" -o "${OUT_DIR}/USBHostLite/rtc_cal.o"
${CC} ${CFLAGS} "${LPC_DIR}/USBHostLite/usbhost_lpc17xx.c" -o "${OUT_DIR}/USBHostLite/usbhost_lpc17xx.o"
${CC} ${CFLAGS} "${LPC_DIR}/USBHostLite/usbhost_ms.c" -o "${OUT_DIR}/USBHostLite/usbhost_ms.o"

echo "=== 2. Linking LPC1768_AIO.axf ==="
${CC} \
  -mcpu=cortex-m3 -mthumb \
  --specs=nano.specs --specs=nosys.specs \
  -L"${PROJECT_ROOT}/CMSISv1p30_LPC17xx/Debug" \
  -L"${PROJECT_ROOT}/FreeRTOS_Library/Debug" \
  -L"${OUT_DIR}" \
  -Xlinker --gc-sections \
  -Xlinker -Map="${OUT_DIR}/LPC1768_AIO.map" \
  -T "${LPC_DIR}/Debug/example01_gnu.ld" \
  -o "${OUT_DIR}/LPC1768_AIO.axf" \
  "${OUT_DIR}/cr_startup_lpc17.o" \
  "${OUT_DIR}/main.o" \
  "${OUT_DIR}/hal_power.o" \
  "${OUT_DIR}/intermcu_protocol.o" \
  "${OUT_DIR}/telemetry_formatter.o" \
  "${OUT_DIR}/sim7600_driver.o" \
  "${OUT_DIR}/ui_display.o" \
  "${OUT_DIR}/storage_log.o" \
  "${OUT_DIR}/flash_log.o" \
  "${OUT_DIR}/flash_nvram.o" \
  "${OUT_DIR}/fat_sd/diskio.o" \
  "${OUT_DIR}/fat_sd/spi_sd_lpc17xx.o" \
  "${OUT_DIR}/fat_sd/ff.o" \
  "${OUT_DIR}/fat_sd/fattime.o" \
  "${OUT_DIR}/fat_sd/option/ccsbcs.o" \
  "${OUT_DIR}/USBHostLite/rtc_cal.o" \
  "${OUT_DIR}/USBHostLite/usbhost_lpc17xx.o" \
  "${OUT_DIR}/USBHostLite/usbhost_ms.o" \
  "${OUT_DIR}/NXP/Drivers/source/lpc17xx_adc.o" \
  "${OUT_DIR}/NXP/Drivers/source/lpc17xx_clkpwr.o" \
  "${OUT_DIR}/NXP/Drivers/source/lpc17xx_gpdma.o" \
  "${OUT_DIR}/NXP/Drivers/source/lpc17xx_gpio.o" \
  "${OUT_DIR}/NXP/Drivers/source/lpc17xx_i2c.o" \
  "${OUT_DIR}/NXP/Drivers/source/lpc17xx_nvic.o" \
  "${OUT_DIR}/NXP/Drivers/source/lpc17xx_pinsel.o" \
  "${OUT_DIR}/NXP/Drivers/source/lpc17xx_ssp.o" \
  "${OUT_DIR}/NXP/Drivers/source/lpc17xx_uart.o" \
  "${OUT_DIR}/NXP/Drivers/source/lpc17xx_wdt.o" \
  "${OUT_DIR}/NXP/Drivers/source/lpc17xx_libcfg_default.o" \
  -lCMSISv1p30_LPC17xx -lFreeRTOS_Library -lm

echo "=== 3. Assembling Sector 0 Boot Stub ==="
${AS} -mcpu=cortex-m3 -mthumb "${SCRIPTS_DIR}/boot_stub.s" -o "${OUT_DIR}/boot_stub.o"
${LD} -T "${SCRIPTS_DIR}/boot_stub.ld" "${OUT_DIR}/boot_stub.o" -o "${OUT_DIR}/boot_stub.elf"
${OBJCOPY} -O binary "${OUT_DIR}/boot_stub.elf" "${OUT_DIR}/boot_stub.bin"
python3 "${SCRIPTS_DIR}/lpc_checksum.py" "${OUT_DIR}/boot_stub.bin"
${OBJCOPY} -I binary -O ihex --change-addresses 0x00000000 "${OUT_DIR}/boot_stub.bin" "${OUT_DIR}/boot_stub.hex"

echo "=== 4. Merging Boot Stub (0x0) + Application (0x18000) ==="
${OBJCOPY} -O binary "${OUT_DIR}/LPC1768_AIO.axf" "${OUT_DIR}/LPC1768_AIO.bin"
${OBJCOPY} -I binary -O ihex --change-addresses 0x00018000 "${OUT_DIR}/LPC1768_AIO.bin" "${OUT_DIR}/LPC1768_AIO_APP.hex"
cat "${OUT_DIR}/boot_stub.hex" | grep -v '^:00000001FF' > "${OUT_DIR}/LPC1768_AIO_FULL.hex"
cat "${OUT_DIR}/LPC1768_AIO_APP.hex" >> "${OUT_DIR}/LPC1768_AIO_FULL.hex"

${SIZE} "${OUT_DIR}/LPC1768_AIO.axf"
echo "=== LPC1768 Build Completed Successfully! ==="
echo "Full Flashing HEX: ${OUT_DIR}/LPC1768_AIO_FULL.hex"
