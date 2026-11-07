#!/bin/bash
cd "$(dirname "$0")/.."
date +%H:%M:%S
P=~/Library/Python/3.9/bin/pyocd
for i in 1 2 3 4 5 6 7 8; do
  if $P flash -t lpc845 -f 1000000 --erase chip LPC845/LPC845_TWS.hex > /tmp/f845.log 2>&1; then echo "flashed on try $i"; tail -2 /tmp/f845.log; break; else echo "try $i: $(tail -1 /tmp/f845.log)"; fi
done
date +%H:%M:%S
