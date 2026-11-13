#!/bin/bash
# dump.sh: while the game runs (go.sh), the next frame's recorded lists (text) and display image (PNG)
# into run_uo/drive/native, and the presented frame (uo_pad.sh dump); prints the files.
cd "$(dirname -- "$0")/../../run_uo/drive" || exit 1
export PATH=$(echo "$PATH" | tr ':' '\n' | grep -v retcomm | paste -sd:)
# All three capture the same frame.
touch native_dump_now native_shot_now
bash ../../tools/uo_pad.sh dump
ls -t native/*.txt | head -1; ls -t native/*.png 2>/dev/null | head -1
