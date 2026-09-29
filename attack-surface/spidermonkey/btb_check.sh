#!/bin/bash

# Branch Target Reuse (BTR)
# April 30th 2026
# Yuhui Zhu

# PoC flow stage 5: BTB control (the end-to-end leak).
# The whole point: does the dispatcher's BHB fingerprinting + iBTB training let a
# mispredicted indirect call reach the reused gadget, leaving a FLUSH+RELOAD
# signal? demo.js reports `avg(cnt)` = the average number of dispatcher slots that
# saw an F+R hit per probe rep. We exercise the "各情况" (modes) the attack runs
# in and gate on a real signal:
#   - spray training (default): trainers sprayed, GC'd, target reused.
#   - fixed training (--fixed-training): pre-JITed callees, no spray/GC/wa churn.
# The spray mode is the full attack, so it alone decides the verdict: its
# avg(cnt) must exceed CNT_MIN. The fixed mode runs for reference.
#
# NB: the F+R timing is only reliable on real hardware with an optimized build
# and a pinned core. On the debug shell here avg(cnt) is 0/NaN, so this reports
# NO-SIGNAL (needs hardware) -- the intended honest gate.
#
# PASS iff spray-mode avg(cnt) > CNT_MIN.
#
# Usage:  JS_CMD=/path/to/js ./btb_check.sh <uarch>   [NR_REPEATS=n CNT_MIN=n]
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE" || exit 2
source "$HERE/common.sh"          # needs $1=uarch; sets TEST_PARAMS (ISA+uarch), PIN_CMD, WASM_MODULE

NR_REPEATS="${NR_REPEATS:-100}"
CNT_MIN="${CNT_MIN:-1}"

# Runs one mode, prints its human line to the terminal, and leaves the parsed
# avg(cnt) in $MODE_CNT (a plain global, so the informational line is never
# swallowed by a command substitution).
run_mode() {  # $1=label  $2...=extra flags
  local label="$1"; shift
  local out cnt
  out="$("${PIN_CMD[@]}" "$JS_CMD" "$JS_SCRIPT" "${TEST_PARAMS[@]}" \
          --repeats "$NR_REPEATS" --wa-mod "$WASM_MODULE" "$@" 2>&1)"
  # demo.js prints "avg(cnt)=X" (X may be NaN when no probe rep saw a hit).
  cnt="$(printf '%s\n' "$out" | grep -oE 'avg\(cnt\)=[0-9.]+' | grep -oE '[0-9.]+' | tail -1)"
  [[ -z "$cnt" ]] && cnt="NaN"
  echo "[btb] mode=$label avg(cnt)=$cnt"
  MODE_CNT="$cnt"
}

gate="0"
for spec in "spray:" "fixed:--fixed-training"; do
  label="${spec%%:*}"; flags="${spec#*:}"
  run_mode "$label" $flags
  if [[ "$label" == "spray" && "$MODE_CNT" != "NaN" ]]; then gate="$MODE_CNT"; fi
done

# echo "[btb] spray avg(cnt)=$gate (threshold CNT_MIN=$CNT_MIN)"
if awk "BEGIN{exit !($gate > $CNT_MIN)}"; then
  echo "[btb] PASS: BTB controlled -- mispredicted call reaches the gadget (F+R signal)"
  exit 0
fi
echo "[btb] NO-SIGNAL: no F+R signal on this host (debug build / no pinned uarch); needs real hardware + opt build"
exit 1
