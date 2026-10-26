#!/usr/bin/env bash
# Drives a running game without window focus (Linux counterpart of uo_pad.ps1).
#   tools/uo_pad.sh --launch [game dir]           start the game wired to run_uo/drive
#   tools/uo_pad.sh "cross wait800 lx=0:1500 dump" send a sequence
# Tokens: name = press for 150 ms; name:ms = hold ms (join with + to hold together, e.g.
# lx=80+ly=240:500); waitN = pause N ms; dump = frame dump, converted to png, path printed.
# Pad names as in uo_pad.ps1; sticks lx= ly= rx= ry= (0..255, 128 centre).
set -u
port=$(cd -- "$(dirname -- "$0")/.." && pwd)
work=$port/run_uo/drive
pad=$work/pad.txt dumps=$work/dumps
mkdir -p "$dumps"
if [[ ${1:-} == --launch ]]; then
    : > "$pad"
    (cd "$port" && VW_GAME_DIR=${2:-${VW_GAME_DIR:?game folder}} VW_DATA_DIR=$port/run_uo VW_UPDATES=0 \
        VW_PAD_FILE=$pad VW_FRAME_DUMP_TRIGGER=$work/dump_now VW_DUMP_DIR=$dumps \
        python3 run.py > "$work/game.log" 2>&1 &)
    echo "started (pad $pad, log $work/game.log)"
    exit 0
fi
for t in ${1:-}; do
    if [[ $t =~ ^wait([0-9]+)$ ]]; then sleep "$(awk "BEGIN{print ${BASH_REMATCH[1]}/1000}")"; continue; fi
    if [[ $t == dump ]]; then
        before=$(ls "$dumps"/output_*.raw 2>/dev/null | wc -l)
        touch "$work/dump_now"
        for _ in $(seq 40); do (( $(ls "$dumps"/output_*.raw 2>/dev/null | wc -l) > before )) && break; sleep 0.15; done
        sleep 0.3
        rm -f "$dumps"/display_*.raw
        python3 -I "$port/tools/raw2png.py" "$(ls -t "$dumps"/output_*.raw | head -1)"
        continue
    fi
    ms=150
    if [[ $t =~ ^(.+):([0-9]+)$ ]]; then t=${BASH_REMATCH[1]} ms=${BASH_REMATCH[2]}; fi
    echo "${t//+/ }" > "$pad"; sleep "$(awk "BEGIN{print $ms/1000}")"; : > "$pad"; sleep 0.35
done
