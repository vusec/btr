#!/bin/bash

# Branch Target Reuse (BTR)
# April 30th 2026
# Yuhui Zhu

source common.sh

NR_REPEATS=1000

echo "${TEST_PARAMS[@]}"
cmd=("${PIN_CMD[@]}" $JS_CMD $JS_SCRIPT "${TEST_PARAMS[@]}" --repeats $NR_REPEATS --wa-mod $WASM_MODULE)
echo ${cmd[@]}

start_ms=$(date +%s%3N)
"${cmd[@]}"
end_ms=$(date +%s%3N)
echo "avg time per repeat: $(echo "scale=3; ($end_ms - $start_ms) / $NR_REPEATS" | bc) ms"