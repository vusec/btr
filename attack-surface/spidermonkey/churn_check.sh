#!/bin/bash

# Branch Target Reuse (BTR)
# April 30th 2026
# Yuhui Zhu

# PoC flow stage 3a: no mid-loop JIT churn.
# force_jit_utils() in demo.js pre-JITs every util function in go()'s call chain
# so nothing compiles DURING the measurement loop (a mid-loop compile fragments
# executable memory and can relocate go(), staling the trained iBTB entries).
# This runs the driver under tools/jit_ev.gdb (which prints [BASELINE]/[TIER-UP]
# per JS compile and demo.js's own `rep=` lines) and post-processes with
# tools/jit_churn.awk, which attributes every non-trainer util compile to the rep
# it happened in. Any such compile at rep>=1 is churn.
#
# PASS iff jit_churn.awk reports nothing (no util compile inside the loop).
#
# Usage:  JS_CMD=/path/to/js ./churn_check.sh <uarch>   [NR_REPEATS=n]
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE" || exit 2
source "$HERE/common.sh"          # needs $1=uarch; sets JS_CMD guard, TEST_PARAMS, WASM_MODULE

NR_REPEATS="${NR_REPEATS:-20}"
LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT

# jit_ev.gdb sets the JIT/wasm breakpoints and ends with `run`; capture its
# combined output (gdb printf + demo.js print) to the log.
gdb --nx -q \
  --iex 'set auto-load safe-path /' --iex 'set pagination off' \
  --ex 'set confirm off' \
  --command=tools/jit_ev.gdb \
  --ex quit \
  --args "$JS_CMD" "$JS_SCRIPT" "${TEST_PARAMS[@]}" \
      --wa-mod "$WASM_MODULE" --repeats "$NR_REPEATS" \
  > "$LOG" 2>&1

CHURN="$(awk -f tools/jit_churn.awk "$JS_SCRIPT" "$LOG")"
if [[ -n "$CHURN" ]]; then
  echo "$CHURN"
  n=$(printf '%s\n' "$CHURN" | grep -c .)
  echo "[churn] FAIL: $n mid-loop util JIT compile(s) over $NR_REPEATS reps"
  exit 1
fi
echo "[churn] PASS: no mid-loop util JIT compiles over $NR_REPEATS reps"
exit 0
