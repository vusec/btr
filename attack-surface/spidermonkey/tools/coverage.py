# Branch Target Reuse (BTR)
# April 30th 2026
# Yuhui Zhu

# Allocation-coverage report for the BTR SpiderMonkey PoC: what fraction of the
# JS trainer chunks land on a useful spot in the wasm target module. One tool for
# both arches -- the arch difference collapses into a scoring MODE over the live
# pool layout, not separate code:
#   --mode trampoline (default): a trainer that lands ANYWHERE in a pool reaches
#       the gadget via a trampoline (the [ gadget | trampoline* ] pool invariant),
#       so it counts if its target-relative offset is in [pool_base, pool_end).
#   --mode gadget: only a DIRECT landing on the gadget works (landing mid-gadget
#       executes garbage), so it counts only if the offset == pool_base (slot 0).
# Scoring is point-based (the trainer's chunk base); the per-trainer size in the
# log is not needed, so no trainer-size constant is hardcoded.
#
# The pool layout (codeBase-relative [base, end) per pool) comes from a live probe
# -- tools/wasm_layout.gdb's [POOL] pass (--layout) -- so nothing about the target
# geometry is hardcoded here either.
#
# jit_ev.gdb emits:
#   [BASELINE] <filename>:<lineno>  code=<base>  size=<len>   (every JS baseline compile)
#   [WASM STUBS]     code=<base>  size=<len>                  (wasm shared stubs)
#   [WASM BASELINE]  code=<base>  size=<len>                  (wasm baseline tier)
#   [WASM ION]       code=<base>  size=<len>                  (wasm optimized tier)
#
# It is a *general* probe (it does not filter to the demo), so the demo-specific
# selection lives here:
#   - Training chunks: the dynamically-created trainer functions. Each is built
#     by the `new Function(...)` call inside make_trainer() in demo.js, so
#     SpiderMonkey names its script "demo.js line <N> > Function", where <N> is
#     the source line of that `new Function(` call. <N> drifts whenever demo.js
#     changes, so instead of hardcoding it we PARSE demo.js to recover it.
#   - Target chunk: the leak-snippet wasm module. jit_ev's wasm hook
#     also catches the tiny empty pre-warm module (demo.js:635, a few bytes),
#     so we keep only [WASM BASELINE] blocks larger than TARGET_MIN_SIZE.

import argparse
import re
import sys

# Everything below this is a pre-warm / empty wasm module, not the real target.
TARGET_MIN_SIZE = 0x1000

# The demo function whose `new Function(...)` call spawns each trainer. The name
# is stable across edits; the source line is not, which is why we look it up.
TRAINER_FACTORY = "make_trainer"

# [BASELINE] <filename>:<lineno>  code=<hex>  size=<hex>
# filename may contain spaces (e.g. "demo.js line 428 > Function") and the
# self-hosted variety contains a ':' too, so the greedy (.*) backtracks onto the
# last ':<digits>  code=' which is the real lineno separator.
BASELINE_RE = re.compile(
    r"^\[BASELINE\] (.*):(\d+)\s+code=(0x[0-9a-fA-F]+)\s+size=(0x[0-9a-fA-F]+)")
WASM_BASELINE_RE = re.compile(
    r"^\[WASM BASELINE\]\s+code=(0x[0-9a-fA-F]+)\s+size=(0x[0-9a-fA-F]+)")

# tools/wasm_layout.gdb [POOL] pass: one line per literal pool it enumerated from
# the whole code buffer (idx is a running counter), giving each pool's
# codeBase-relative [base, end). end=NONE means the probe found the pool's slot 0
# but could not confirm its upper bound (the last slot did not validate as a
# trampoline back to slot 0 -- $nslot/$slot/target mismatch), so we refuse to
# score rather than trust a wrong range.
POOL_RE = re.compile(
    r"^\[POOL\] idx=(\d+) base=(NONE|0x[0-9a-fA-F]+)(?: end=(NONE|0x[0-9a-fA-F]+))?")


def parse_layout(layout_path):
    """Parse wasm_layout.gdb [POOL] lines into codeBase-relative (base, end)
    pool ranges. Raises ValueError on any unestablished bound or if no pools
    were found (a broken probe must fail loudly, not silently undercount)."""
    ranges = []
    with open(layout_path, "r") as f:
        for line in f:
            m = POOL_RE.match(line)
            if not m:
                continue
            idx, base, end = m.group(1), m.group(2), m.group(3)
            if base == "NONE" or end == "NONE" or end is None:
                raise ValueError(
                    f"{layout_path}: pool idx {idx} has an unestablished "
                    f"bound (base={base} end={end}) -- target/layout mismatch, "
                    f"refusing to score")
            ranges.append((int(base, 16), int(end, 16)))
    if not ranges:
        raise ValueError(
            f"{layout_path}: no [POOL] lines -- run wasm_layout.gdb with $nslot>0")
    return ranges


def find_trainer_line(js_path, func_name=TRAINER_FACTORY):
    """Line number of the first `new Function(` inside function <func_name>."""
    decl_re = re.compile(rf"\bfunction\s+{re.escape(func_name)}\s*\(")
    in_func = False
    with open(js_path, "r") as f:
        for lineno, line in enumerate(f, start=1):
            if not in_func:
                if decl_re.search(line):
                    in_func = True
            if in_func and "new Function(" in line:
                return lineno
    return None


def iter_targets(log_path, trainer_name_suffix):
    """Yield (target_base, target_len, training_chunks) for each target wasm
    compile, where training_chunks are the trainer chunks seen since the
    previous target."""
    training_chunks = set()
    with open(log_path, "r") as f:
        for line in f:
            m = BASELINE_RE.match(line)
            if m:
                fn, lineno = m.group(1), int(m.group(2))
                if lineno == 1 and fn.endswith(trainer_name_suffix):
                    train_base = int(m.group(3), 16)
                    # JS script leaves 0x10b of metadata at the head of the chunk, so we need to skip it.
                    training_chunks.add(train_base - 0x10)
                continue

            m = WASM_BASELINE_RE.match(line)
            if m:
                target_base = int(m.group(1), 16)
                target_len = int(m.group(2), 16)
                # Skip the tiny empty pre-warm module; only the leak-snippet is the target.
                if target_len < TARGET_MIN_SIZE:
                    continue
                yield target_base, target_len, training_chunks
                training_chunks = set()


def reaches_gadget(off, pool_ranges, mode):
    """Does a trainer whose target-relative offset is `off` reach the gadget?
    trampoline: it lands somewhere in a pool -> a trampoline carries it in
    (off in [base, end)). gadget: only a direct landing on slot 0 works
    (off == base). See the module header."""
    for pool_base, pool_end in pool_ranges:
        if mode == "gadget":
            if off == pool_base:
                return True
        elif pool_base <= off < pool_end:
            return True
    return False


def run(log_path, js_path, layout_path, mode):
    """Drive the whole report for both arches. Scores each trainer chunk against
    the live pool layout under `mode` (see reaches_gadget / the module header)."""
    pool_ranges = parse_layout(layout_path)
    print(f"layout: {layout_path} ({len(pool_ranges)} pools), mode={mode}")
    for i, (pool_base, pool_end) in enumerate(pool_ranges):
        print(f"pool {i}: 0x{pool_base:x} ~ 0x{pool_end:x}")

    trainer_line = find_trainer_line(js_path)
    if trainer_line is None:
        print(f"error: no `new Function(` found in {TRAINER_FACTORY}() of {js_path}",
              file=sys.stderr)
        sys.exit(1)
    # SpiderMonkey names a `new Function` script "<file> line <N> > Function".
    trainer_name_suffix = f"line {trainer_line} > Function"
    print(f"trainer: {TRAINER_FACTORY}() -> {js_path}:{trainer_line} "
          f"(matching '... {trainer_name_suffix}')")

    rates = []
    for target_base, target_len, training_chunks in iter_targets(log_path, trainer_name_suffix):
        nr_train_chunks = len(training_chunks)
        nr_correct_align = sum(
            reaches_gadget(train_base - target_base, pool_ranges, mode)
            for train_base in training_chunks)
        rate = nr_correct_align / nr_train_chunks if nr_train_chunks > 0 else 0
        print(f"rate: {rate:.2%} ({nr_correct_align}/{nr_train_chunks})")
        rates.append(rate)

    effective = rates[5:]
    if effective:
        avg = sum(effective) / len(effective)
        print(f"average rate (skipping first 5): {avg:.2%} over {len(effective)} samples")
    return rates


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="BTR allocation-coverage report (both arches; the arch "
                    "difference is the --mode, not separate code).")
    ap.add_argument("--mode", choices=["trampoline", "gadget"], default="trampoline",
                    help="trampoline: land anywhere in a pool (default); "
                         "gadget: land directly on the gadget (slot 0)")
    ap.add_argument("--layout", required=True,
                    help="wasm_layout.gdb [POOL] dump (codeBase-relative pool bounds)")
    ap.add_argument("log", help="allocation log from jit_ev.gdb")
    ap.add_argument("js", nargs="?", default="demo.js",
                    help="demo JS (for the trainer line; default: demo.js)")
    a = ap.parse_args(argv)
    run(a.log, a.js, a.layout, a.mode)


if __name__ == "__main__":
    main()
