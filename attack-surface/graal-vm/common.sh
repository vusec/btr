#!/bin/bash

# Branch Target Reuse (BTR)
# April 30th 2026
# Yuhui Zhu

# The native image every experiment script drives. Override from the command
# line to run a different build, e.g. `CMD=target/embedding-dbg ./btb-control.sh zen4`.
CMD="${CMD:-target/embedding}"
REPLACE_RATE_SCRIPT="replace_rate.py"

# Test parameters come in two layers, assembled into TEST_PARAMS at the end of
# this file. Each element is one argv token, so callers pass "${RUN_PARAMS[@]}"
# (quoted) and vm.py's parse_args sees "--nr-bh" and "256" as separate arguments.
#   RUN_PARAMS    -- the chunk geometry: how many trainer and target chunks each
#                    iteration compiles.
#   UARCH_PARAMS  -- the branch-history geometry the BHB fingerprinting uses.
# btb-control.sh sweeps the branch-history layer itself, so it passes RUN_PARAMS
# plus its own --nr-bh / --nr-bcond / --nr-bcond-taken.

RUN_PARAMS=(
    --nr-trainer 4
    --nr-target 4
)

UARCH_PARAMS=(
    --nr-bh 256
    --nr-bcond 512
    --nr-bcond-taken 256
)

PINCORE_X3_PIXEL8=8
PINCORE_A76_RPI5=4
PINCORE_ZEN4=8
PINCORE_RAPTORCOVE_14900K=2
PINCORE_LIONCOVE_285K=2

if [[ ! -x $CMD ]]; then
    echo "Error: CMD ($CMD) does not exist or is not executable."
    echo "Build the native image first: mvn clean -DskipTests -Pnative -Pisolated package"
    exit 1
fi

if [[ $# -lt 1 ]]; then
    echo "Error: args[0] = [x3|a76|zen4|raptorcove|lioncove]"
    exit 1
fi

UARCH=$1

if [[ "$UARCH" == "x3" ]]; then
    ARCH=aarch64
    PINCORE=$PINCORE_X3_PIXEL8
elif [[ "$UARCH" == "a76" ]]; then
    ARCH=aarch64
    PINCORE=$PINCORE_A76_RPI5
elif [[ "$UARCH" == "zen4" ]]; then
    ARCH=x86_64
    PINCORE=$PINCORE_ZEN4
elif [[ "$UARCH" == "raptorcove" ]]; then
    ARCH=x86_64
    PINCORE=$PINCORE_RAPTORCOVE_14900K
elif [[ "$UARCH" == "lioncove" ]]; then
    ARCH=x86_64
    PINCORE=$PINCORE_LIONCOVE_285K
else
    echo "Error: args[0] = [x3|a76|zen4|raptorcove|lioncove]"
    exit 1
fi

# The launcher prefix the timing-sensitive stages put in front of the native
# image. On aarch64 the run also takes the highest real-time priority, so the
# measurement loop holds the pinned core for its whole slice. That needs
# CAP_SYS_NICE, so it is probed once here and used when the probe succeeds;
# PIN_RT=off keeps the plain taskset prefix for comparison runs.
PIN_RT="${PIN_RT:-auto}"
PIN_CMD=(taskset -c "$PINCORE")
if [[ "$ARCH" == "aarch64" && "$PIN_RT" != "off" ]]; then
    PIN_RT_CMD=(nice -n -20 chrt -f 99)
    if "${PIN_RT_CMD[@]}" true 2>/dev/null; then
        PIN_CMD=("${PIN_RT_CMD[@]}" "${PIN_CMD[@]}")
    else
        echo "[pin] nice -20 / SCHED_FIFO 99 unavailable (needs CAP_SYS_NICE);" \
             "measuring at normal priority -- rerun as root for the tuned setup" >&2
    fi
fi

# The command line the scripts that keep the branch-history geometry fixed pass
# to the native image verbatim.
TEST_PARAMS=("${RUN_PARAMS[@]}" "${UARCH_PARAMS[@]}")
