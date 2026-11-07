#!/bin/bash
R=$(cd "$(dirname "$0")/.." && pwd); O=${OUT:-/tmp/aio_bench}/precommit2; mkdir -p $O
cd $R/LPC1768/boot/test
F="-std=gnu11 -g -O1 -fsanitize=address,undefined -I.. -I../.."
cc $F -o $O/bt1768 boot_test.c ../boot_core.c ../fw_journal.c && $O/bt1768 | tail -1
cc $F -DFW_TARGET_LPC845 -o $O/bt845 boot_test.c ../boot_core.c ../fw_journal.c && $O/bt845 | tail -1
cc $F -DFW_TARGET_LPC845 -I../../../LPC845/boot -o $O/ctrl ctrl_test.c ../../fw_update.c ../../fw_ctrl.c ../../../LPC845/boot/fw845_recv.c ../boot_core.c ../fw_journal.c && $O/ctrl ../../../LPC845/sensor.hex | tail -1
cc $F -o $O/ut update_test.c ../../fw_update.c ../boot_core.c ../fw_journal.c
$O/ut $R/LPC1768/Debug/fw_bihar.hex 1 | tail -1
$O/ut ${OUT:-/tmp/aio_bench}/b699/KSNDMC_TRG/LPC1768/Debug/fw_ksn_trg.hex 0 | tail -1
$O/ut ${OUT:-/tmp/aio_bench}/b699/KSNDMC_TWS/LPC1768/Debug/fw_ksn_tws.hex 2 | tail -1
