#!/bin/bash
# bench.sh [VW_NATIVE value] [seconds]: frame rate of the New Game opening, uncapped.
# 0 is the shadPS4 renderer alone, 1 both renderers, own the native renderer owning the frame.
# Prints the presenter's 5-second "Frame stats" lines from New Game on.
cd "$(dirname -- "$0")/../.." || exit 1
export VW_FRAME_STATS=1 VW_VSYNC=0
bash tools/native/go.sh "${1:-own}" >/dev/null 2>&1 </dev/null
sleep 3
start=$(grep -ac 'Frame stats' run_uo/drive/game.log)
bash tools/uo_pad.sh "down wait1500 cross" >/dev/null
sleep "${2:-60}"
grep -a 'Frame stats' run_uo/drive/game.log | tail -n +$((start + 1)) | sed 's/;.*//'
pkill -x vwport
