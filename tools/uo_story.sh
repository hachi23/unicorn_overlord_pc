#!/usr/bin/env bash
# Plays Unicorn Overlord from New Game through the prologue into chapter 1 (Linux), driving the
# pad through the port's pad file (VW_PAD_FILE, tools/uo_pad.sh) and dumping a frame at each
# checkpoint, so a run can be checked afterwards (the chapter title card, #49).
#
# usage: tools/uo_story.sh [game dir]   (VW_TEXT_LANGUAGE, VW_PROBE etc. pass through)
# Frames: run_uo/drive/dumps (uo_pad.sh). The run continues even when a step misses: look at the
# frames to see where it went off.
#
# Stick values: 0..255, 128 centre; the battle cursor moves about 1 px per ms of full stick
# after a short ramp (short pushes move less). The cursor's hit point is the sword's tip.
set -u
port=$(cd -- "$(dirname -- "$0")/.." && pwd)
pad() { bash "$port/tools/uo_pad.sh" "$*" >/dev/null; }
# advance <presses> [ms] [dump every]: press Cross, dumping every n-th press
advance() {
    local n=$1 ms=${2:-2500} every=${3:-0}
    for i in $(seq "$n"); do
        pad "cross wait$ms"
        if (( every && i % every == 0 )); then pad dump; fi
    done
}

# seek <name> <steps...>: small stick steps until the unit panel reads <name> (OCR, uo_ocr.sh).
# Each step is a uo_pad.sh token such as lx=0:110 (left); the panel shows the unit under the
# cursor's tip, so a spiral of steps finds a unit near the cursor.
seek() {
    local name=$1 f; shift
    for step in "" "$@"; do
        [[ -n $step ]] && pad "$step wait900"
        pad dump
        f=$(ls -t "$port/run_uo/drive/dumps"/output_*.png | head -1)
        if bash "$port/tools/uo_ocr.sh" "$f" | grep -qi "$name"; then return 0; fi
    done
    echo "uo_story: $name not found" >&2; return 1
}
U=ly=0:110 D=ly=255:110 L=lx=0:110 R=lx=255:110
SPIRAL=($U $R $D $D $L $L $U $U $U $R $R $R $D $D $D $D $L $L $L $L $U $U $U $U $U)

pkill -x vwport; sleep 2
find "$port/run_uo/drive/dumps" -name 'output_*' -delete 2>/dev/null
bash "$port/tools/uo_pad.sh" --launch "${1:-${VW_GAME_DIR:?game folder}}" >/dev/null
sleep 36                                   # warnings, logos

# Title: the first Cross skips the opening movie, the second is "press any button"; the menu
# takes input only after that (a d-pad press earlier counts as "any button").
pad "cross wait6000 cross wait3000 down wait1500 cross wait15000"   # New Game
echo "uo_story: opening"
advance 60 2000                            # the queen's garden, Valmore's attack
pad dump

# Tutorial battle, the defence of the capital. Two tutorial pages, then the order
# "Select Ilenia's unit, then select Valmore's unit to move to them".
pad "right wait1200 cross wait1500 cross wait2500 dump"
# Ilenia (the orange-ringed 6) is a little up and right of the starting cursor; Valmore (the
# armored 9) is below. Select her, Move, then Valmore (Select, Battle).
pad "ly=0:120 wait300 lx=255:100 wait900"
seek "enia" "${SPIRAL[@]}"
pad "cross wait1500 cross wait1500"
pad "ly=255:300 wait400 lx=0:120 wait1200"
seek "almor" "${SPIRAL[@]}"
pad "cross wait1500 cross wait4000 dump"
echo "uo_story: prologue battle"
# Encounters ask "Battle" (Cross); between fights Cross opens Ilenia's Move menu, which Circle
# closes again. The battle ends in Valmore's scene, then the story runs to chapter 1.
for _ in $(seq 30); do pad "cross wait3500 cross wait3500 circle wait800 circle wait800"; done
echo "uo_story: story"
for i in $(seq 140); do pad "cross wait2200"; (( i % 2 )) || pad dump; done
echo "uo_story: done (chapter 1: the title card \"The Unicorn Ring\" is among the last frames)"
