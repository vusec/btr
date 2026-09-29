# Attack Surface Analysis - SpiderMonkey

This folder contains the code for the attack surface analysis on SpiderMonkey from Firefox 143.

## Setup

Clone Firefox and check out the Firefox 143 tag:

```sh
git clone https://github.com/mozilla-firefox/firefox.git
cd firefox
git checkout DEVEDITION_143_0b9_BUILD1
git apply $PATH_TO_THIS_REPO/attack-surface/spidermonkey/jsshell/firefox143.patch
```

Copy the pre-built Mozilla build config from the artifact and set `MOZCONFIG`:

```sh
cp -r $PATH_TO_THIS_REPO/attack-surface/spidermonkey/jsshell/.mozbuild $HOME/.mozbuild
export MOZCONFIG=$HOME/.mozbuild/jsshell-arm   # aarch64
export MOZCONFIG=$HOME/.mozbuild/jsshell-x86   # x86_64
```

Setup toolchain and build the JS shell:

```sh
mach bootstrap && mach build
```

Set `JS_CMD` to the compiled `js` binary before running any experiment:

```sh
export JS_CMD=/path/to/firefox/obj-${BUILD_PATH}/dist/bin/js
```

The WebAssembly leak-snippet targets under `wasm/` are prebuilt; regenerate them
with the unified generator (it auto-compiles and writes a sidecar), e.g.:

```sh
python tools/make_target_wasm.py --arch x86_64 --encoding v128 --out-dir wasm
```

## PoC flow (`make`)

`Makefile` is the single front door: it builds the target and gates the four
properties the attack relies on before measuring the leak. Each stage is a target
you can run standalone or via `make all`, and prints `PASS` / `FAIL` / `NO-SIGNAL`:

| stage             | requirement                                   | tool |
|-------------------|-----------------------------------------------|------|
| `make abi`        | 1. callee stack ABI == gadget stack reads     | `abi_check.sh` (disnative of the real demo.js F+R callees) |
| `make tune`       | 2. auto-probe a page-aligned, max-pool target | `tools/tune_target.py` (verify only) |
| `make target`     | 2. …and deploy it to `wasm/`                  | `tools/tune_target.py --deploy` |
| `make churn`      | 3a. no mid-loop JIT churn                     | `churn_check.sh` |
| `make bailouts`   | 3b. no steady-state Ion bailouts              | `bailouts_check.sh` |
| `make reuse`      | 4. trainers GC'd, target reuses their pages   | `inspect_reuse.sh` |
| `make btb`        | 5. BTB control → F+R signal                   | `btb_check.sh` |
| `make all`        | run every stage, print a summary              | — |
| `make frcalib`    | standalone: calibrate F+R hit/miss timing     | `fr_calib.sh` (run before stage 5 on a new host) |

```sh
export JS_CMD=/path/to/obj-debug-x86_64-.../dist/bin/js
make all UARCH=zen4 PAGES=8          # UARCH: x3 a76 zen4 raptorcove lioncove
make tune PAGES=8                    # just re-derive nr_loads for an 8-page target
make frcalib UARCH=zen4              # measure this host's F+R hit/miss latencies
```

`make frcalib` sits outside `make all`: it is a per-host measurement you run and
read yourself. Stage 5 counts a probe as a hit when its reload time is below
`FR_THRESHOLD` (70 ns in `demo.js`), which only holds while that number falls in
the gap between the host's cache-hit and cache-miss reload distributions.
`fr_calib.sh` measures both distributions with the real `demo.js` constants and
F+R buffer, prints mean / variance / percentiles / histograms, writes
`results/fr_calib.json`, and reports the threshold they support — pass it to
`demo.js` as `--fr-threshold <ns>`. A `FAIL` there means the primitive cannot
resolve a cache hit on this host, so no stage-5 verdict would be meaningful.

`make tune` auto-derives `nr_loads` by JITing candidates and fitting
`codeLength(nr_loads)`, so the target fills exactly `PAGES` × 64 KB exec pages and
lands padding-free (page-aligned `codeBase`). Stage 5 gates on a real FLUSH+RELOAD
signal: it shows up in `--fixed-training` mode even here, while the full spray/reuse
signal needs an optimized build on real hardware.

## Run the experiments

All scripts source `common.sh` and pass its `TEST_PARAMS` to `demo.js` verbatim.
`common.sh` builds that array from two layers: `ARCH_PARAMS_*` holds the per-ISA
trainer geometry and literal-pool layout (`--nr-trainer`, `--len-trainer`,
`--nr-target`, `--pool-layout`), `UARCH_PARAMS_*` holds the per-microarchitecture
branch-history geometry (`--nr-bh`, `--nr-bcond`, `--nr-bcond-taken`). Pass the
target microarchitecture as the first argument; it selects both layers:

|   Argument   |                 CPU                 |
|--------------|-------------------------------------|
| `x3`         | Cortex-X3 (Pixel 8)                 |
| `a76`        | Cortex-A76 (Raspberry Pi 5)         |
| `zen4`       | AMD Zen 4  (Ryzen 9 7950X)          |
| `raptorcove` | Intel Raptor Cove (Core i9-14900K)  |
| `lioncove`   | Intel Lion Cove (Core Ultra 9 285K) |

### `rate.sh` — measure the chunk-reuse rate

Runs the experiment 1000 times and prints average time per repeat.

```sh
./rate.sh <uarch>
# e.g.:
./rate.sh zen4
```

### `btb_control.sh` — sweep BTB control parameters

Sweeps `nr_entry`, `len_bh`, and `nr_bcond_taken` and reports how many iBTB entries stay controlled (`avg(cnt)`): the top-5 combinations per mode, and the maximum each mode reaches.

The second argument selects the modes (comma-separated, default `fixed,spray`):

| mode | flags | what it measures |
|---|---|---|
| `fixed` | `--fixed-training` | the upper bound on controllable entries, with pre-JITed callees and no GC/WA churn |
| `churn` | `--fixed-training --no-skip-gc --no-skip-wa` | how many of those entries survive the interference the attack carries: the same training, with the GC storm and the wasm target reload between training and probing |
| `spray` | — | the full attack: sprayed trainers, GC, target reuse |

`NR_REPEATS` (default 100) sets the probe reps per point, `SETTLE` (default 1) the pause between points.

```sh
./btb_control.sh <uarch> [modes]
# e.g.:
./btb_control.sh raptorcove
./btb_control.sh raptorcove fixed,churn,spray
./btb_control.sh raptorcove churn
```

### `inspect_reuse.sh` — inspect allocation reuse with GDB

Attaches GDB using `tools/jit_ev.gdb` to record allocation coverage, then probes the target's live pool layout (`tools/wasm_layout.gdb`) and post-processes the log with `tools/coverage.py`.

```sh
./inspect_reuse.sh <uarch>
# e.g.:
./inspect_reuse.sh a76
```
