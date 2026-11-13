#!/bin/bash
# tour.sh MODE MOVE OUT_DIR [steps...]: open Load Game from the title menu with VW_NATIVE=MODE,
# move MOVE rows from where the list opens (the last slot saved; negative: up; the list
# wraps), load, then run each step (a tools/uo_pad.sh token string) and capture a frame after it
# into OUT_DIR/NN.png. The same steps with mode 0 and own give comparable pictures.
cd "$(dirname -- "$0")/../.." || exit 1
mode=$1 move=$2 out=$3; shift 3
mkdir -p "$out"
bash tools/native/go.sh "$mode" >/dev/null 2>&1
pad() { bash tools/uo_pad.sh "$*" >/dev/null; }
pad "down wait600 down wait600 cross wait4000"
key=down; (( move < 0 )) && key=up move=$((-move))
for _ in $(seq "$move"); do pad "$key wait500"; done
pad "cross wait2500 left wait500 cross wait15000"
n=0
for step in "$@"; do
    pad "$step"
    pad dump
    cp "$(ls -t run_uo/drive/dumps/output_*.png | head -1)" "$out/$(printf %02d $n).png"
    n=$((n + 1))
done
grep -a 'Native: no converted\|not handled\|did not bind\|reads render\|Frame stats' run_uo/drive/game.log | sed 's/;.*//' | tail -8
pkill -x vwport
