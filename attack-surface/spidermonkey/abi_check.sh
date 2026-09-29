#!/bin/bash

# Branch Target Reuse (BTR)
# April 30th 2026
# Yuhui Zhu

# PoC flow stage 1: trainer/callee stack ABI.
# Two halves must agree for the reused gadget to read the right arguments:
#   callee side : the REAL demo.js F+R callees (dummy_callee_reload1/2), warmed to
#                 Ion, read arg0 and arg2 from fixed frame-pointer slots.
#                 Recovered live with tools/abi_check.js -> disnative() (loads
#                 demo.js unmodified).
#   gadget side : the deployed target's gadget reads the same two slots off the
#                 stack pointer the caller left behind. Encoded in the sidecar
#                 first64.
#
#   x86_64  : callee rbp+0x28 / rbp+0x38 == rsp+0x20 / rsp+0x30 at call entry
#             (`call` pushed the return address), gadget first64 =
#             mov rax,[rsp+0x20] ; mov rdx,-0x2000000000000.
#   aarch64 : the prologue pushes x30 and x29 through the pseudo stack pointer
#             x28 and sets x29 = x28 - 16, so callee x29+40 / x29+56 == entry
#             x28+0x18 / x28+0x28 (`bl` keeps the return address in x30), gadget
#             first64 = ldr x0,[x28,#0x18] ; eor x0,x0,#0xfffe000000000000.
#
# PASS iff both offsets appear for BOTH callees AND the sidecar first64 matches.
#
# The uarch key selects the ISA the same way every other stage does: common.sh
# maps it to $ARCH and to the $WASM_MODULE this stage checks the sidecar of.
#
# Usage:  JS_CMD=/path/to/js ./abi_check.sh <uarch>
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE" || exit 2
source "$HERE/common.sh"          # needs $1=uarch; sets JS_CMD guard, ARCH, WASM_MODULE
DEMO="$HERE/demo.js"

case "$ARCH" in
  aarch64)
    ARG0_SLOT='[x29, #40]'
    ARG2_SLOT='[x29, #56]'
    GADGET_FIRST64="0xd24f3800f9400f80"
    ;;
  *)
    ARG0_SLOT='0x28(%rbp)'
    ARG2_SLOT='0x38(%rbp)'
    GADGET_FIRST64="0x00ba482024448b48"
    ;;
esac

fail() { echo "[abi] FAIL: $*"; exit 1; }

# --- callee side (disnative of the REAL demo.js callees) ---
DIS="$("$JS_CMD" "$HERE/tools/abi_check.js" "$DEMO" 2>&1)" || fail "abi_check.js run failed:\n$DIS"
# disnative emits the shell's own ISA, so a shell built for another arch is
# reported as such rather than as a missing arg slot.
if echo "$DIS" | grep -q '(%rbp)'; then JS_ARCH=x86_64; else JS_ARCH=aarch64; fi
[[ "$JS_ARCH" == "$ARCH" ]] \
  || fail "\$JS_CMD is a $JS_ARCH shell; $WASM_MODULE needs a $ARCH one"
# Two disnative dumps (reload1, reload2); both must be Ion and read the same slots.
n_ion=$(echo "$DIS" | grep -ci 'backend=ion')
[[ $n_ion -ge 2 ]] || fail "callees did not both reach Ion (backend=ion count=$n_ion):\n$DIS"

arg0_n=$(echo "$DIS" | grep -cF "$ARG0_SLOT")
arg2_n=$(echo "$DIS" | grep -cF "$ARG2_SLOT")
echo "[abi] callee side ($ARCH): arg0@$ARG0_SLOT hits=$arg0_n  arg2@$ARG2_SLOT hits=$arg2_n (over 2 callees)"
[[ $arg0_n -ge 2 && $arg2_n -ge 2 ]] \
  || fail "arg offsets not present in both callees (arg0=$arg0_n arg2=$arg2_n)"

# --- gadget side (sidecar first64) ---
SIDECAR="${WASM_MODULE%.wasm}.sidecar.json"
[[ -f "$SIDECAR" ]] || fail "sidecar not found: $SIDECAR (build/deploy the target first)"
FIRST64="$(python - "$SIDECAR" <<'PY'
import json, sys
print(json.load(open(sys.argv[1]))["gadget"]["first64"])
PY
)"
echo "[abi] gadget side: sidecar first64=$FIRST64 (expect $GADGET_FIRST64)"
[[ "$FIRST64" == "$GADGET_FIRST64" ]] \
  || fail "gadget first64 mismatch -- deployed target does not carry the $ARCH arg ABI"

echo "[abi] PASS: callee reads arg0@$ARG0_SLOT / arg2@$ARG2_SLOT; gadget mirrors it"
exit 0
