#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# parity.sh OUT_DIR SCENARIO [key=value...]: run one scenario with this build (out/vwport) and
# with a reference build, and put the frames side by side, to check that a feature looks the
# same with the native renderer as it did before (the shadPS4 renderer of v1.5.2 and 0b09c1a).
#
#   SCENARIO  title           the title menu
#             opening         New Game, then the first story text boxes (fonts, text language)
#             options         the title menu's System > Options screen (the PC rows, button icons)
#             load:MOVE       Load Game, MOVE rows from the last saved slot, then the battle map
#   key=value  vw.ini lines for both runs (e.g. button_icons=xbox); vw.ini is restored afterwards.
#
# Environment: VW_GAME_DIR (required), VW_PARITY_REF (the reference build, default
# out/ref/vwport_shadps4: build commit 0b09c1a with tools/native/build.sh and copy out/vwport
# there), and whatever run.py reads: VW_TEXT_LANGUAGE, VW_ASPECT_RATIO, VW_RENDER_SCALE,
# VW_LANGUAGES_DIR, VW_PATCHES...
#
# Output: OUT_DIR/native/NN.png, OUT_DIR/reference/NN.png, OUT_DIR/side_NN.png (native left) and
# one line per frame with the RMSE between the two (animated scenes never match exactly: look).
set -u
cd "$(dirname -- "$0")/../.." || exit 1
out=${1:?OUT_DIR} scenario=${2:?SCENARIO}; shift 2
ref=${VW_PARITY_REF:-out/ref/vwport_shadps4}
[[ -x $ref ]] || { echo "parity: no reference build at $ref (VW_PARITY_REF)"; exit 1; }
: "${VW_GAME_DIR:?set VW_GAME_DIR to the game folder}"
export VW_VSYNC=0
ini=run_uo/vw.ini
cp "$ini" "$out.vw.ini.bak" 2>/dev/null || { mkdir -p "$out" && cp "$ini" "$out.vw.ini.bak"; }
restore() { mv -f "$out.vw.ini.bak" "$ini"; pkill -x vwport; pkill -x "$(basename "$ref")"; }
trap restore EXIT
pad() { bash tools/uo_pad.sh "$*" >/dev/null; }
capture() { pad dump; cp "$(ls -t run_uo/drive/dumps/output_*.png | head -1)" "$1"; }

run() {   # run DIR [VW_PROBE]
    local dir=$1 n=0
    mkdir -p "$dir"; rm -f "$dir"/*.png
    cp "$out.vw.ini.bak" "$ini"
    for kv in "${settings[@]}"; do
        grep -v "^${kv%%=*}=" "$ini" > "$ini.tmp"; echo "$kv" >> "$ini.tmp"; mv "$ini.tmp" "$ini"
    done
    VW_PROBE=${2:-} bash tools/native/go.sh >/dev/null 2>&1
    pgrep -x "$(basename "${2:-vwport}")" >/dev/null || { echo "parity: the game did not reach the title ($dir)"; return 1; }
    case $scenario in
    title) ;;
    opening)
        pad "down wait1500 cross wait15000"
        for _ in 1 2 3; do capture "$dir/$(printf %02d $n).png"; n=$((n + 1)); pad "cross wait2500 cross wait2500"; done ;;
    options)
        pad "down wait600 down wait600 down wait600 down wait600 cross wait3000"
        capture "$dir/$(printf %02d $n).png"; n=$((n + 1))
        pad "cross wait3000" ;;
    load:*)
        local move=${scenario#load:} key=down
        pad "down wait600 down wait600 cross wait4000"
        (( move < 0 )) && key=up move=$((-move))
        for _ in $(seq "$move"); do pad "$key wait500"; done
        pad "cross wait2500 left wait500 cross wait15000" ;;
    *) echo "parity: unknown scenario $scenario"; return 1 ;;
    esac
    capture "$dir/$(printf %02d $n).png"
    grep -a "^Native: .*\(not handled\|no converted\|failed\)\|Text language\|Languages:" run_uo/drive/game.log | sort -u | head -5
    pkill -x vwport; pkill -x "$(basename "$ref")"; sleep 2
}

settings=("$@")
echo "== native (out/vwport)"; run "$out/native" || exit 1
echo "== reference ($ref)"; run "$out/reference" "$PWD/$ref" || exit 1
for f in "$out"/native/*.png; do
    b=$(basename "$f"); r="$out/reference/$b"
    [[ -f $r ]] || continue
    montage "$f" "$r" -tile 2x1 -geometry +4+0 "$out/side_$b"
    printf '%s RMSE %s\n' "$b" "$(compare -metric RMSE "$f" "$r" null: 2>&1)"
done
