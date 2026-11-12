#!/bin/bash
# gh.sh <Script.java> <out.txt> [args...]
script=$1; out=$2; shift 2
GHIDRA_HEADLESS_MAXMEM=8G "${GHIDRA_HOME:-/opt/ghidra}/support/analyzeHeadless" "${UO_GHIDRA_PROJECT:?folder of the Ghidra project with eboot_ghidra.elf}" uo -process eboot_ghidra.elf -noanalysis -readOnly \
  -scriptPath "$(dirname "$0");$HOME/unicorn_overlord_pc/tools/re/ghidra_scripts" -postScript "$script" "$@" > "$out.log" 2>&1
python3 -I - "$out.log" "$out" "${script%.java}" <<'PY'
import sys, re
log, out, name = sys.argv[1:]
txt = open(log, errors='replace').read()
parts = re.split(r'INFO  ' + re.escape(name) + r'\.java> ', txt)[1:]
res = []
for p in parts:
    p = re.split(r' \(GhidraScript\)\s*\n', p, 1)[0]
    res.append(p)
open(out, 'w').write('\n'.join(res))
PY
