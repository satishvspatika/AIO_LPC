#!/bin/bash
R=$(cd "$(dirname "$0")/.." && pwd); S=${OUT:-/tmp/aio_bench}; L=$R/LPC1768; T=$R/tools/arm-toolchain/bin/arm-none-eabi
INC=(-I$L -I$R/CMSISv1p30_LPC17xx/inc -I$R/FreeRTOS_Library/include -I$R/FreeRTOS_Library/portable -I$R/FreeRTOS_Library/demo_code -I$L/NXP/Drivers/include -I$L/fat_sd -I$L/USBHostLite)
mkdir -p $S; C=$S/ci; rm -rf $C; mkdir -p $C; cd $C
for f in ctrl_isp ctrl_isp_core fw_update fw_ctrl fw_app boot/fw_journal main ui_display storage_log hal_power intermcu_protocol telemetry_formatter sim7600_driver flash_nvram flash_log fat_sd/ff fat_sd/diskio fat_sd/spi_sd_lpc17xx fat_sd/option/ccsbcs NXP/Drivers/source/lpc17xx_ssp NXP/Drivers/source/lpc17xx_uart; do [ -f $L/$f.c ] || continue; b=$(basename $f); $T-gcc -c -fcallgraph-info=su -mcpu=cortex-m3 -mthumb -O2 -fcommon -DUNIT_CFG=${1:-2} -DDEBUG -I$L/Debug -D__USE_CMSIS=CMSISv1p30_LPC17xx "${INC[@]}" $L/$f.c -o $b.o 2>/dev/null; done; ls *.ci | wc -l
A=$L/Debug/LPC1768_AIO.axf
python3 "$R/bench/stack_callgraph.py" $C $A $T-objdump
