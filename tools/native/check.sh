#!/bin/bash
# check.sh [captures] [interval]: the standard comparison run. Restarts the game with the native
# renderer, captures the title menu, starts New Game and captures the opening every [interval]
# seconds; writes the reference/native grid to $OUT (default run_uo/drive/native/pairs.png) and
# prints the renderer's new log lines.
cd "$(dirname -- "$0")/../.." || exit 1
out=${OUT:-run_uo/drive/native/pairs.png}
bash tools/native/build.sh 2>&1 | grep -q "build ok" || { echo "build failed: out/static-build.log"; exit 1; }
bash tools/native/go.sh "${NATIVE_MODE:-1}" >/dev/null 2>&1 </dev/null
sleep 3
timeout 20 bash tools/native/dump.sh </dev/null >/dev/null 2>&1
bash tools/uo_pad.sh "down wait1500 cross wait8000" >/dev/null
for _ in $(seq "${1:-5}"); do
    timeout 20 bash tools/native/dump.sh </dev/null >/dev/null 2>&1
    sleep "${2:-5}"
done
/usr/bin/python3 -I tools/native/pairs.py "$out"
grep -a 'Native' run_uo/drive/game.log | grep -v ' frames;\|Native: vs \|written to\|frame behind' | tail -20
