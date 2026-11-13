#!/bin/bash
# ndump.sh: ask for a recorded frame (text), the native image and the reference image; prints the files.
cd "$(dirname -- "$0")/../../run_uo/drive" || exit 1
export PATH=$(echo "$PATH" | tr ':' '\n' | grep -v retcomm | paste -sd:)
# Both renderers capture the same moment: the native triggers and the reference dump together.
touch native_dump_now native_shot_now
bash ../../tools/uo_pad.sh dump
ls -t native/*.txt | head -1; ls -t native/*.png 2>/dev/null | head -1
