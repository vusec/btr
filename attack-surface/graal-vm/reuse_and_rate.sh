#!/bin/bash

# Branch Target Reuse (BTR)
# April 30th 2026
# Yuhui Zhu

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE" || exit 2
source "$HERE/common.sh"

"${PIN_CMD[@]}" $CMD "${TEST_PARAMS[@]}" --repeats 100 2> >(python $REPLACE_RATE_SCRIPT)
