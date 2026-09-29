#!/bin/bash

# Branch Target Reuse (BTR)
# April 30th 2026
# Yuhui Zhu

source common.sh

NR_REPEATS=10

# Logging is enabled here (before loading the breakpoint script) because
# jit_ev.gdb runs the target itself -- it ends with `run` -- so its
# output must already be captured to the log.
GDB_CMD_SETTINGS=(
    "set pagination off"
    "set confirm off"
    "set print pretty on"
    "set debuginfod enabled off"
    "set logging file $ALLOC_COVERAGE_LOG"
    "set logging enabled on"
)

gdb_cmd=(gdb --nx -q)
# These must run BEFORE the --args binary is loaded, so use --iex (init-eval):
#  - auto-load safe-path: silence the js-gdb.py auto-load refusal and let its
#    pretty-printers load.
#  - pagination off: the auto-load message is printed while loading the binary,
#    i.e. before any --ex runs, so pagination must already be off or gdb stalls
#    at the "---Type <return> to continue---" pager prompt.
GDB_INIT_SETTINGS=(
    "set auto-load safe-path /"
    "set pagination off"
)
for cmd in "${GDB_INIT_SETTINGS[@]}"; do
    gdb_cmd+=(--iex "$cmd")
done
# Setup gdb environment.
for cmd in "${GDB_CMD_SETTINGS[@]}"; do
    gdb_cmd+=(--ex "$cmd")
done
# jit_ev.gdb sets the JIT/wasm breakpoints AND runs the target.
gdb_cmd+=(--command=tools/jit_ev.gdb)
gdb_cmd+=(--ex quit --args $JS_CMD $JS_SCRIPT "${TEST_PARAMS[@]}" --wa-mod $WASM_MODULE --repeats $NR_REPEATS)

rm -f $ALLOC_COVERAGE_LOG
"${gdb_cmd[@]}"

# Scoring is layout-driven for both arches: probe the deployed module's live
# literal-pool layout once with tools/wasm_layout.gdb ([POOL] pass) and hand the
# dump to coverage via --layout. wasm_layout.gdb sources its arch module by CWD,
# so it must run from tools/ ($WASM_MODULE is relative to here -> ../ from
# tools/). Probe params are matched to the deployed module from its sidecar when
# present ($pat/$slot/$nslot/$goff); otherwise the shared v128 defaults apply, which fit
# what wasm/ ships for x86_64. Both modules in wasm/ carry a sidecar with a
# committed gadget, so both arches get matched probe params; the aarch64 lattice
# geometry is tuned on ARM hardware (see tools/README.md).
ALLOC_LAYOUT_LOG=layout.log
# Override from the command line, e.g. GRID_STRIDE=0x400 ./inspect_reuse.sh a76
layout_ex=(-ex "set \$grid=$GRID_STRIDE" -ex "set \$arch=\"$ARCH\"")
SIDECAR="${WASM_MODULE%.wasm}.sidecar.json"
if [[ -f "$SIDECAR" ]]; then
    while IFS= read -r ex; do layout_ex+=(-ex "$ex"); done < <(python - "$SIDECAR" <<'PY'
import json, sys
d = json.load(open(sys.argv[1])); g = d["gadget"]; c = d["config"]
print(f'set $pat={g["first64"]}')
print(f'set $slot={g["bytes"] // g["chunks"]}')
print(f'set $nslot={c["nr_loads"]}')
# gadget_off arrived with the [ tramp* | gadget | tramp* ] pool; a sidecar
# generated before it has the gadget at slot 0.
print(f'set $goff={c.get("gadget_off", 0)}')
PY
)
fi
( gdb --nx -q -batch "${layout_ex[@]}" -x tools/wasm_layout.gdb \
    --args "$JS_CMD" tools/probe_load.js "$WASM_MODULE" ) > $ALLOC_LAYOUT_LOG 2>&1
COV_OUT="$(python $ALLOC_COVERAGE_SCRIPT $ALLOC_COVERAGE_LOG $JS_SCRIPT --layout $ALLOC_LAYOUT_LOG)"
echo "$COV_OUT"

# PoC flow stage 4 gate: trainers must be GC'd and their pages reused by the
# target, i.e. coverage.py's average reuse rate (skipping the first 5 warmup reps)
# is > 0. PASS/FAIL + exit code make this drop-in for `make reuse` / CI.
RATE="$(printf '%s\n' "$COV_OUT" | grep -oE 'average rate \(skipping first 5\): [0-9.]+%' | grep -oE '[0-9.]+' | tail -1)"
if [[ -z "$RATE" ]]; then
    echo "[reuse] FAIL: no coverage rate produced (probe/layout error -- see above)"
    exit 1
elif awk "BEGIN{exit !($RATE > 0)}"; then
    echo "[reuse] PASS: trainers reuse target pages (avg reuse rate ${RATE}% > 0)"
    exit 0
else
    echo "[reuse] FAIL: avg reuse rate ${RATE}% == 0 (target did not land on freed trainer pages)"
    exit 1
fi