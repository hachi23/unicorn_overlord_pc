#!/bin/bash
# Builds the native-renderer worktree (static) and installs out/vwport. Prints errors only.
cd "$(dirname -- "$0")/../.." || exit 1
P=$(echo "$PATH" | tr ':' '\n' | grep -v retcomm | paste -sd:)
if env -u CC -u CXX -u CMAKE_SYSROOT -u LDFLAGS PATH="$P" bash build.sh --static > out/nb.log 2>&1; then
    pkill -x vwport; sleep 1; cp out/static/vwport out/vwport && echo "build ok"
else
    grep -E 'error|Error|undefined' out/static-build.log | head -30; tail -5 out/nb.log; exit 1
fi
