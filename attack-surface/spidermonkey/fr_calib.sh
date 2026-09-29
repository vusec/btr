#!/bin/bash

# Branch Target Reuse (BTR)
# April 30th 2026
# Yuhui Zhu

# FLUSH+RELOAD timing calibration.
# Run this on the measurement host BEFORE stage 5 (btb_check.sh). demo.js counts
# a dispatcher slot as leaking when its reload time is below FR_THRESHOLD, so
# that number has to sit in the gap between this host's cache-hit and cache-miss
# reload distributions. This measures both distributions with the real demo.js
# constants and F+R buffer (tools/fr_calib.js), prints mean / variance /
# percentiles / histograms, and reports the threshold the measurement supports.
#
# The result is written to results/fr_calib.json and echoed as the demo.js flag
# to pass:  --fr-threshold <n>.  Stage 5 keeps demo.js's own default unless that
# flag is given, so applying the calibration stays an explicit choice.
#
# PASS iff the hit and miss distributions separate: gap >= MIN_GAP ns AND the
# recommended threshold misclassifies <= MAX_ERR_PCT of the samples. A FAIL
# means the F+R primitive cannot tell a hit from a miss on this host, which
# makes any stage-5 verdict meaningless.
#
# Usage:  JS_CMD=/path/to/js ./fr_calib.sh <uarch>
#         [SAMPLES=20000 FR_WARMUP=1000 MIN_GAP=20 MAX_ERR_PCT=1]
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE" || exit 2
source "$HERE/common.sh"          # needs $1=uarch; sets JS_CMD guard, PIN_CMD

SAMPLES="${SAMPLES:-20000}"
FR_WARMUP="${FR_WARMUP:-1000}"
MIN_GAP="${MIN_GAP:-20}"
MAX_ERR_PCT="${MAX_ERR_PCT:-1}"
OUT_JSON="$HERE/results/fr_calib.json"

fail() { echo "[frcalib] FAIL: $*"; exit 1; }

OUT="$("${PIN_CMD[@]}" "$JS_CMD" "$HERE/tools/fr_calib.js" "$HERE/demo.js" \
        --samples "$SAMPLES" --warmup "$FR_WARMUP" 2>&1)" \
  || fail "fr_calib.js run failed:
$OUT"

# The report lines go to the terminal; the machine-readable last line is split
# off into the sidecar.
printf '%s\n' "$OUT" | grep -v '^FR_CALIB_JSON='
JSON="$(printf '%s\n' "$OUT" | grep '^FR_CALIB_JSON=' | tail -1)"
JSON="${JSON#FR_CALIB_JSON=}"
[[ -n "$JSON" ]] || fail "fr_calib.js produced no FR_CALIB_JSON line:
$OUT"

mkdir -p "$HERE/results"
printf '%s\n' "$JSON" > "$OUT_JSON"
echo "[frcalib] wrote $OUT_JSON"

read -r GAP REC ERR DEMO < <(python - "$OUT_JSON" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
print(d["gap"], d["recommended"], d["err_pct_recommended"], d["demo_threshold"])
PY
)

echo "[frcalib] gate: gap=${GAP}ns (min ${MIN_GAP}) err=${ERR}% (max ${MAX_ERR_PCT}%)"
awk "BEGIN{exit !($GAP >= $MIN_GAP && $ERR <= $MAX_ERR_PCT)}" \
  || fail "hit and miss reload times do not clear the gate -- at this separation" \
          "the F+R primitive resolves a cache hit too unreliably for a stage-5 verdict"

echo "[frcalib] PASS: hit/miss separate; calibrated threshold=${REC}ns (demo.js default ${DEMO}ns)"
echo "[frcalib] to apply it, add to the demo.js command line:  --fr-threshold $REC"
exit 0
