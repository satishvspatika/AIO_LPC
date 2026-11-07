#!/bin/bash
PY=~/Library/Python/3.9/bin/pyocd
for f in 1m 100k 10k; do for m in attach under-reset; do printf '%-5s %-12s ' $f $m; "$PY" commander -t lpc845 -f $f -M $m -c "read32 0xE000ED00" 2>&1 | grep -vE '^\s*$|Setting SWD' | tail -1 | cut -c1-90; done; done
echo '--- same probe, try as LPC1768 target (in case the cable is on that board)'; "$PY" commander -t lpc1768 -f 100k -M attach -c "read32 0xE000ED00" 2>&1 | tail -1 | cut -c1-90
