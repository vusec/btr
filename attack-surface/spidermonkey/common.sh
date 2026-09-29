#!/bin/bash

# Branch Target Reuse (BTR)
# April 30th 2026
# Yuhui Zhu

JS_SCRIPT=demo.js
NO_SKIP_FLAG="--no-skip-gc --no-skip-wa"
EXTRA_FLAGS=""
ALLOC_COVERAGE_LOG=allocation.log

# Test parameters come in two layers, assembled into TEST_PARAMS at the end of
# this file:
#   ARCH_PARAMS_*  -- per ISA: trainer geometry and literal-pool layout, set by
#                     the JIT backend (x86_64 / aarch64).
#   UARCH_PARAMS_* -- per microarchitecture: the branch-history geometry the BHB
#                     fingerprinting is tuned to.
# Each element is one argv token, so callers pass "${TEST_PARAMS[@]}" (quoted)
# and demo.js's parseArgs sees "--nr-bh" and "4096" as separate arguments.

# ---- per-ISA parameters -----------------------------------------------------

# GRID_STRIDE_* is the BTB landing-grid stride: control flow lands at
# codeBase + n*GRID_STRIDE + LAND_OFF, matching the trainer spray's --len-trainer.
# The aarch64 lattice tuning and wasm_layout.gdb's [GRID] diagnostic consume it;
# x86 trampoline coverage comes from the measured [POOL] path. Override from the
# command line, e.g. `GRID_STRIDE=0x800 ./inspect_reuse.sh zen4`.

GRID_STRIDE_X86_64=0x200
WASM_MODULE_X86_64=wasm/leak_snippet_x86_64_v128.wasm
ARCH_PARAMS_X86_64=(
    --nr-trainer 256
    --nr-target 1
    --len-trainer 12
    # uncomment to enable pool layout instrumenting
    # --pool-layout -0x1300,0x10e00,0,1 
)

GRID_STRIDE_AARCH64=0x600
WASM_MODULE_AARCH64=wasm/leak_snippet_aarch64_f64.wasm
# The aarch64 target uses the lattice layout: its flushed constant pool spills
# into pool_repeat * block_repeat = 42 * 2 islands, one per 0x600 grid point,
# each carrying the gadget at byte offset 0x10 (see the module's .sidecar.json).
# --nr-trainer sprays one trainer per island, covering every landing point.
ARCH_PARAMS_AARCH64=(
    --nr-trainer 84
    --nr-target 1
    --len-trainer 120
)

# ---- per-microarchitecture parameters ---------------------------------------

PINCORE_A76_RPI5=3
UARCH_PARAMS_A76=(
    --nr-bh 2048
    --nr-bcond 64
    --nr-bcond-taken 32
)

PINCORE_X3_PIXEL8=8
UARCH_PARAMS_X3=(
    --nr-bh 2048
    --nr-bcond 128
    --nr-bcond-taken 64
)

PINCORE_ZEN4=8
UARCH_PARAMS_ZEN4=(
    --nr-bh 4096
    --nr-bcond 64
    --nr-bcond-taken 32
)

PINCORE_RAPTORCOVE_14900K=2
UARCH_PARAMS_RAPTORCOVE_14900K=(
    --nr-bh 4096
    --nr-bcond 128
    --nr-bcond-taken 16
)

PINCORE_LIONCOVE_285K=2
UARCH_PARAMS_LIONCOVE_285K=(
    --nr-bh 8192
    --nr-bcond 256
    --nr-bcond-taken 16
)

# Allocation-coverage scoring; the arch difference is the --mode passed to it.
ALLOC_COVERAGE_SCRIPT=tools/coverage.py

if [[ $JS_CMD == "" ]]; then
    echo "Error: JS_CMD is not set."
    exit 1
elif [[ ! -x $JS_CMD ]]; then
    echo "Error: JS_CMD ($JS_CMD) does not exist or not executable."
    exit 1
fi

if [[ $# -lt 1 ]]; then
    echo "Error: args[0] = [a76|x3|zen4|raptorcove|lioncove]"
    exit 1
fi

UARCH=$1

if [[ "$UARCH" == "a76" ]]; then
    ARCH=aarch64
    PINCORE=$PINCORE_A76_RPI5
    UARCH_PARAMS=("${UARCH_PARAMS_A76[@]}")
elif [[ "$UARCH" == "x3" ]]; then
    ARCH=aarch64
    PINCORE=$PINCORE_X3_PIXEL8
    UARCH_PARAMS=("${UARCH_PARAMS_X3[@]}")
elif [[ "$UARCH" == "zen4" ]]; then
    ARCH=x86_64
    PINCORE=$PINCORE_ZEN4
    UARCH_PARAMS=("${UARCH_PARAMS_ZEN4[@]}")
elif [[ "$UARCH" == "raptorcove" ]]; then
    ARCH=x86_64
    PINCORE=$PINCORE_RAPTORCOVE_14900K
    UARCH_PARAMS=("${UARCH_PARAMS_RAPTORCOVE_14900K[@]}")
elif [[ "$UARCH" == "lioncove" ]]; then
    ARCH=x86_64
    PINCORE=$PINCORE_LIONCOVE_285K
    UARCH_PARAMS=("${UARCH_PARAMS_LIONCOVE_285K[@]}")
else
    echo "Error: args[0] = [a76|x3|zen4|raptorcove|lioncove]"
    exit 1
fi

if [[ "$ARCH" == "x86_64" ]]; then
    GRID_STRIDE=${GRID_STRIDE:-$GRID_STRIDE_X86_64}
    WASM_MODULE=$WASM_MODULE_X86_64
    ARCH_PARAMS=("${ARCH_PARAMS_X86_64[@]}")
elif [[ "$ARCH" == "aarch64" ]]; then
    GRID_STRIDE=${GRID_STRIDE:-$GRID_STRIDE_AARCH64}
    WASM_MODULE=$WASM_MODULE_AARCH64
    ARCH_PARAMS=("${ARCH_PARAMS_AARCH64[@]}")
fi

# The launcher prefix the timing-sensitive stages (btb, F+R calibration, rate)
# put in front of the js shell. On aarch64 the run also takes the highest
# real-time priority, so the measurement loop holds the pinned core for its whole
# slice. That needs CAP_SYS_NICE, so it is probed once here and used when the
# probe succeeds; PIN_RT=off keeps the plain taskset prefix for comparison runs.
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

# The driver command line every stage script passes to demo.js verbatim.
TEST_PARAMS=("${ARCH_PARAMS[@]}" "${UARCH_PARAMS[@]}")
