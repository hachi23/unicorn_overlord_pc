#!/bin/bash
# go.sh [seconds]: restart the game and stop at the title menu (after [seconds], default 42, of boot).
# VW_PROBE=<binary> runs another build, e.g. an older one for comparing pictures.
cd "$(dirname -- "$0")/../.." || exit 1
export PATH=$(echo "$PATH" | tr ':' '\n' | grep -v retcomm | paste -sd:)
pkill -x vwport; sleep 1
mkdir -p run_uo/drive/native
rm -f run_uo/drive/native/*.txt run_uo/drive/native/*.png run_uo/drive/dumps/output_*
export VW_NATIVE_SHOT_TRIGGER=$PWD/run_uo/drive/native_shot_now VW_NATIVE_DUMP_TRIGGER=$PWD/run_uo/drive/native_dump_now VW_NATIVE_DUMP_DIR=$PWD/run_uo/drive/native VW_PREBUILT=1
bash tools/uo_pad.sh --launch "${VW_GAME_DIR:?set VW_GAME_DIR to the game folder (CUSA36381)}" >/dev/null
sleep ${1:-42}
bash tools/uo_pad.sh "cross wait6000 cross wait4000"
pgrep -x vwport >/dev/null && echo "at title" || { echo "game died"; tail -20 run_uo/drive/game.log; }
