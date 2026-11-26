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
python3 $R/bench/rtc_check_test.py $R | tail -1
for t in 0 1 2; do cc $F -DUNIT_CFG=$t -I$R/LPC1768 -o $O/sid$t $R/LPC1768/tests/station_id_test.c $R/LPC1768/telemetry_formatter.c && $O/sid$t | tail -1; done
cc $F -I$R/LPC1768 -o $O/npt $R/LPC1768/tests/net_policy_test.c $R/LPC1768/net_policy.c && $O/npt | tail -1
cc $F -I$R/LPC1768 -o $O/smst $R/LPC1768/tests/sms_cmd_test.c $R/LPC1768/sms_cmd.c && $O/smst | tail -1
# Firmware over the air: the download logic (pretend server, card and clock), and the modem reply
# parser taken as it stands from the driver
cc $F -DOTA_STOP_BEFORE_SLOT_S_FOR_TEST=330 -I$R/LPC1768/tests/host_stubs -I$R/LPC1768 -o $O/otat $R/LPC1768/tests/ota_test.c $R/LPC1768/ota.c && $O/otat | tail -1
sed -n '/^static size_t __attribute__((noinline)) get_collect/,/^const char \*sim7600_http_get/p' $R/LPC1768/sim7600_driver.c | sed '$d' > $O/get_collect.inc
cc $F -I$O -o $O/otag $R/LPC1768/tests/ota_get_test.c && $O/otag | tail -1
# Health report (v6.9.60): built from a pretend SLOTS.CSV
cc $F -I$R/LPC1768/tests/host_stubs -I$R/LPC1768 -o $O/hlt $R/LPC1768/tests/health_test.c $R/LPC1768/health.c && $O/hlt | tail -1
# 7.02: the SD backlog slot after slot against a pretend server, from the firmware's own code
# (storage_log.c's backlog section, main.c post_backlog_record and the slot's backlog loop)
sed -n '/Backlog Records Management on SD Card/,/^\/\* Firmware files/p' $R/LPC1768/storage_log.c | sed '1,2d;$d' > $O/bk_store.inc
sed -n '/^static bool post_backlog_record/,/^}/p' $R/LPC1768/main.c > $O/bk_post.inc
sed -n '/^            uint8_t backlogs_sent = 0;/,/^            storage_backlog_cleanup_if_all_sent();/p' $R/LPC1768/main.c > $O/bk_loop.inc
cc $F -Wno-unused-function -I$O -o $O/bkt $R/LPC1768/tests/backlog_test.c && $O/bkt | tail -1
# CTRL v1.18: the rain debounce (LPC845 main.c debounce_rfpin) against rain gauge waveforms
sed -n '/^#define RF_HOLDOFF_CALLS/,/^void Pin_init/p' $R/LPC845/main.c | sed '$d' > $O/rf_debounce.inc
cc $F -I$O -o $O/rdt $R/LPC845/tests/rain_debounce_test.c && $O/rdt | tail -1
