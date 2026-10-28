#!/bin/bash

# Branch Target Reuse (BTR)
# April 30th 2026
# Yuhui Zhu

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE" || exit 2
source "$HERE/common.sh"

# Black-run warm-up iterations before the measured reps; ~30 settles the slow
# outermost framework fns (run_iteration/main_loop). Override with WARMUP_REPS.
WARMUP_REPS="${WARMUP_REPS:-30}"

"${PIN_CMD[@]}" $CMD "${TEST_PARAMS[@]}" --warmup-reps "$WARMUP_REPS" --repeats 100 2> >(python $REPLACE_RATE_SCRIPT)
