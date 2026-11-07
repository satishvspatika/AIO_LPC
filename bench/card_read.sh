#!/bin/bash
# Reads the bench unit's SD card on the Mac: error log, today's boots/restarts/slot ends, last slots and record.
# Usage: card_read.sh [HH:MM]   (log lines from that time on; default 00:00)
# Optional: COPY=path/to/fw_x.hex card_read.sh  copies an update file to the card and checks it.
V="/Volumes/NO NAME"
for i in $(seq 1 25); do [ -d "$V" ] && break; /bin/sleep 1; done
date
[ -d "$V" ] || { echo NOT_MOUNTED; ls /Volumes; exit 1; }
FROM=${1:-00:00}
L="$V/LOG/$(date +%y%m%d).TXT"
echo "--- ERRORS.TXT"; cat "$V/ERRORS.TXT" 2>/dev/null | cut -c1-140
echo "--- $L from $FROM"; grep -a -E "=== BOOT|RESTARTED|Restarted by a fault|SLOT_END|STACK\]" "$L" 2>/dev/null | awk -v f="$FROM" '$2>=f' | cut -c1-130
echo "--- SLOTS.CSV"; tail -4 "$V/SLOTS.CSV" | cut -c1-90
echo "--- newest record"; tail -1 "$(ls -t "$V"/DATA/*.TXT | head -1)"
if [ -n "$COPY" ]; then cp -X "$COPY" "$V/" && cmp "$COPY" "$V/$(basename "$COPY")" && echo "COPIED $(basename "$COPY")"; fi
sync; diskutil eject "$V"
