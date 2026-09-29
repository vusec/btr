#!/bin/bash

# Branch Target Reuse (BTR)
# April 30th 2026
# Yuhui Zhu

# PoC flow stage 3b: no steady-state Ion bailouts.
# Thin wrapper over tools/parse_bailouts.py: it runs the driver under the debug
# shell's bailout spew (IONFLAGS=bailouts,bl-bails), classifies each bailout, and
# attributes it to a phase. Warmup bailouts (FirstExecution while Ion tiers up in
# the first few reps) are benign; the tool exits non-zero (verdict SUSPICIOUS)
# only on a bailout past --warmup or a non-FirstExecution bailout inside the loop.
#
# PASS iff parse_bailouts.py exits 0 (verdict CLEAN).
#
# Usage:  JS_CMD=/path/to/js ./bailouts_check.sh <uarch>   [NR_REPEATS=n WARMUP=n]
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE" || exit 2
source "$HERE/common.sh"          # needs $1=uarch; sets JS_CMD guard, TEST_PARAMS, WASM_MODULE

NR_REPEATS="${NR_REPEATS:-50}"
WARMUP="${WARMUP:-10}"

JS_CMD="$JS_CMD" python tools/parse_bailouts.py --warmup "$WARMUP" -- \
    "${TEST_PARAMS[@]}" \
    --wa-mod "$WASM_MODULE" --repeats "$NR_REPEATS"
rc=$?
if [[ $rc -eq 0 ]]; then
  echo "[bailouts] PASS: measurement loop is bailout-clean (warmup=$WARMUP)"
else
  echo "[bailouts] FAIL: steady-state / non-warmup bailouts (see report above)"
fi
exit $rc
