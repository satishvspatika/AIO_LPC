#!/bin/bash
R=$(cd "$(dirname "$0")/.." && pwd); O=${OUT:-/tmp/aio_bench}/precommit2; mkdir -p $O
cd $R/LPC1768/boot/test
F="-std=gnu11 -g -O1 -fsanitize=address,undefined -I.. -I../.."
cc $F -o $O/bt1768 boot_test.c ../boot_core.c ../fw_journal.c && $O/bt1768 | tail -1
cc $F -DFW_TARGET_LPC845 -o $O/bt845 boot_test.c ../boot_core.c ../fw_journal.c && $O/bt845 | tail -1
cc $F -DFW_TARGET_LPC845 -I../../../LPC845/boot -o $O/ctrl ctrl_test.c ../../fw_update.c ../../fw_ctrl.c ../../../LPC845/boot/fw845_recv.c ../boot_core.c ../fw_journal.c && $O/ctrl ../../../LPC845/ctrl.hex | tail -1
cc $F -o $O/ut update_test.c ../../fw_update.c ../boot_core.c ../fw_journal.c
$O/ut $R/LPC1768/Debug/fw_bihar.hex 1 | tail -1
# The KSNDMC builds live outside the repo: name them with KSN_TRG_HEX / KSN_TWS_HEX
for pair in "${KSN_TRG_HEX:-}:0:KSNDMC_TRG" "${KSN_TWS_HEX:-}:2:KSNDMC_TWS"; do
    hex=${pair%%:*}; rest=${pair#*:}; type=${rest%%:*}; name=${rest#*:}
    if [ -n "$hex" ] && [ -f "$hex" ]; then
        $O/ut "$hex" "$type" | tail -1
    else
        echo "SKIPPED $name update test: set ${name/KSNDMC_/KSN_}_HEX to its fw_*.hex"
    fi
done
cc $F -DFW_TARGET_LPC845 -I$R/LPC1768 -I$R/LPC1768/boot -o $O/cit $R/LPC1768/tests/ctrl_isp_test.c $R/LPC1768/ctrl_isp_core.c $R/LPC1768/fw_update.c $R/LPC1768/boot/boot_core.c $R/LPC1768/boot/fw_journal.c && $O/cit $R/LPC845/ctrl.hex $R/LPC845/LPC845_BOOT.bin $R/LPC845/LPC845_TWS.bin | tail -1
python3 $R/bench/frame_compat_test.py $R | tail -1
for t in 0 1 2; do cc $F -DUNIT_CFG=$t -I$R/LPC1768 -o $O/sid$t $R/LPC1768/tests/station_id_test.c $R/LPC1768/telemetry_formatter.c && $O/sid$t | tail -1; done
cc $F -I$R/LPC1768 -o $O/npt $R/LPC1768/tests/net_policy_test.c $R/LPC1768/net_policy.c && $O/npt | tail -1
cc $F -I$R/LPC1768 -o $O/smst $R/LPC1768/tests/sms_cmd_test.c $R/LPC1768/sms_cmd.c && $O/smst | tail -1
