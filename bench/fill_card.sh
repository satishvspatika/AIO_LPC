#!/bin/bash
# Fills the SD card mounted at /Volumes/NO NAME for the full-card test, in FILLnn.BIN files of at most
# 3.9 GB (FAT32 limit 4 GB), leaving LEAVE_KB free (default 0). "fill_card.sh clean" removes them.
V="/Volumes/NO NAME"
[ -d "$V" ] || { echo "card not mounted"; exit 1; }
if [ "$1" = "clean" ]; then
    rm -f "$V"/FILL*.BIN && sync && df -k "$V" | tail -1
    exit 0
fi
LEAVE_KB=${LEAVE_KB:-0}
n=0
while :; do
    free_kb=$(df -k "$V" | tail -1 | awk '{print $4}')
    want_kb=$((free_kb - LEAVE_KB))
    [ "$want_kb" -le 64 ] && break
    [ "$want_kb" -gt 3900000 ] && want_kb=3900000
    f=$(printf "%s/FILL%02d.BIN" "$V" $n)
    mkfile -n "${want_kb}k" "$f" 2>/dev/null || dd if=/dev/zero of="$f" bs=1k count="$want_kb" 2>/dev/null
    n=$((n + 1))
    [ $n -gt 40 ] && break
done
sync
df -k "$V" | tail -1
