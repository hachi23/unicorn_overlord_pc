#!/bin/bash
# bench.sh [seconds]: frame rate of the New Game opening, uncapped. Prints the flip thread's
# 5-second "Frame stats" lines from New Game on. VW_PROBE=<binary> measures another build.
cd "$(dirname -- "$0")/../.." || exit 1
export VW_FRAME_STATS=1 VW_VSYNC=0
bash tools/native/go.sh >/dev/null 2>&1 </dev/null
sleep 3
start=$(grep -ac 'Frame stats' run_uo/drive/game.log)
bash tools/uo_pad.sh "down wait1500 cross" >/dev/null
sleep "${1:-60}"
grep -a 'Frame stats' run_uo/drive/game.log | tail -n +$((start + 1)) | sed 's/;.*//'
pkill -x vwport
