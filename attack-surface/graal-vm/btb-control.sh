#!/bin/bash

# Branch Target Reuse (BTR)
# April 30th 2026
# Yuhui Zhu

# Sweep the BHB/iBTB geometry (nr_entry x len_bh x nr_bcond_taken) and report how
# many iBTB entries the attack keeps under control. The native image prints
# `hit0: N` = the average number of histories that saw an F+R hit per repeat, so
# hit0 is the controlled-entry count for that geometry.

# Two modes:
#   fixed  --skip-reuse
#          Train and reload back to back, with the trainer chunks left mapped.
#          The upper bound: the largest entry count the BHB fingerprinting can
#          address at all.
#   churn  (no --skip-reuse)
#          Same training, with the GC reclaim, the target-chunk compile onto the
#          freed addresses and the dummy dispatches back in the loop. How many of
#          those entries survive the interference the attack carries.

# Usage:  ./btb-control.sh <uarch> [modes]
#   modes  comma-separated subset of fixed,churn (default: fixed,churn).

# Environment:
#   NR_REPEATS  probe reps per point (default 10)
#   SETTLE      seconds between points (default 1)

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE" || exit 2
source "$HERE/common.sh"

TEST_NR_ENTRY=(16 32 64 128 256 512 1024 2048 4096 8192)
TEST_LEN_BH=(64 128 256 512)
TEST_NR_BH_TAKEN=(16 32 48 64 96 128 192 256)

NR_REPEATS="${NR_REPEATS:-10}"
SETTLE="${SETTLE:-1}"

# ---- mode selection ---------------------------------------------------------

IFS=',' read -r -a MODES <<< "${2:-fixed,churn}"

mode_flags() {  # $1=mode -> the vm.py flags that define it
    case "$1" in
        fixed) echo "--skip-reuse" ;;
        churn) echo "" ;;
        *)     return 1 ;;
    esac
}

for mode in "${MODES[@]}"; do
    if ! mode_flags "$mode" >/dev/null; then
        echo "Error: unknown mode '$mode'; args[1] = [fixed|churn] (comma-separated)"
        exit 1
    fi
done

declare -A BEST BESTGEO

# ---- sweep ------------------------------------------------------------------

for mode in "${MODES[@]}"; do
    FLAGS="$(mode_flags "$mode")"
    results=()

    echo ""
    echo "=== mode=$mode uarch=$UARCH ($ARCH) flags: ${FLAGS:-<churn>} ==="

    for nr_entry in ${TEST_NR_ENTRY[@]}; do
        for len_bh in ${TEST_LEN_BH[@]}; do
            for taken in ${TEST_NR_BH_TAKEN[@]}; do
                if [[ $taken -ge $len_bh ]]; then
                    continue
                fi
                output=$("${PIN_CMD[@]}" $CMD "${RUN_PARAMS[@]}" --nr-bh $nr_entry --nr-bcond $len_bh --nr-bcond-taken $taken $FLAGS --repeats $NR_REPEATS 2>/dev/null | grep "hit0")
                if [[ "$output" =~ hit0:[[:space:]]([0-9]+) ]]; then
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
                echo "nr_entry=$nr_entry, len_bh=$len_bh, taken=$taken, cnt=$val"
                sleep "$SETTLE"
            done
        done
    done

    echo ""
    echo "=== mode=$mode: top 5 combinations by cnt ==="
    printf '%s\n' "${results[@]+"${results[@]}"}" | sort -t' ' -k4 -rn | head -5 |
    while read -r e l t v; do
        echo "nr_entry=$e, len_bh=$l, taken=$t, cnt=$v"
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
