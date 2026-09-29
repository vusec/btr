# SpiderMonkey attack-surface tooling

Helpers for building and verifying the transient-execution leak gadgets used
in the BTR SpiderMonkey PoC. All commands below assume you run from this
`tools/` directory with `$JS_CMD` pointing at the Firefox 143 debug jsshell.

## Training chunks and target chunks

The victim's mispredicted branch lands at `codeBase + n*grid + 0x10` for an `n` the
attacker does not control: `grid` is the BTB landing stride and `+0x10` the entry offset
inside a trainer chunk, both fixed by the JS JIT and derived below. Both sides of the attack
are built around that landing equation — the wasm module makes every landing point reach the
gadget, the JS trainers put an entry on every landing point.

### Wasm module -- BTB landing target

The gadget lives in a literal pool, so the pool policy of each JIT backend decides how much
control there is over its address:

- x86-64 flushes the literal pool when the compilation task ends, so several functions
  share one pool and the gadget address is not directly controllable. Its grid, `0x200`,
  comes from the `--len-trainer 12` chunk stride.
- arm64 flushes every `0x400` bytes, so a set of generator parameters places each pool
  island on a chosen address. Its grid, `0x600`, is that ISA's minimum island spacing.

To keep both compatible, every pool is encoded with the invariant
`[ trampoline* | gadget | trampoline* ]`: one self-contained OOB-read gadget at byte offset
`gadget_off`, every other slot a branch back to it. Displacements are pool-local, so
byte-identical copies work wherever the allocator places them, and the two layouts differ
only in where the landing arrives:

- **trampoline** — `gadget_off = 0`; any landing inside a pool trampolines to the gadget,
  so the gadget's position does not matter and the layout does not depend on the grid.
- **lattice** — `gadget_off = 0x10`; one island is tuned onto each grid point, and control
  flow arrives `LAND_OFF = 0x10` past the island start (`JitCodeHeader` + `CodeAlignment`,
  derived on the training side below), so the gadget sits there to be landed on directly.
  x86 cannot make pools that small.

The invariant also makes a pool self-describing from its sidecar: knowing the slot size,
the slot count and `gadget_off`, a probe finds the gadget by its `first64`, steps back
`gadget_off` to slot 0, jumps straight to the last slot, and confirms it is still a
trampoline — recovering the pool's bounds without hardcoded geometry or walking.
`wasm_layout.gdb`'s `[POOL]` pass checks this and emits the bounds; `coverage.py` consumes
them.

### JS functions -- BTB training chunks

Trainers are generated from a template, each a fresh `Function` behind a unique comment id,
with its compiled length controlled through `--len-trainer`. The pool policies above demand
different lengths: `0x200` on x86-64, which trains a trampoline under the *trampoline*
layout, and `0x600` on arm64, which trains the gadget at the head of each island under the
*lattice* layout. A sprayed trainer with index `i` then sits at offset `i*grid`, so the
spray covers one landing point per trainer.

The `+0x10` in the landing equation is where a trainer's code actually starts inside its
chunk, and it comes from the JIT linker rather than from anything the attacker chooses.
`JitCodeHeader` is one `JitCode*` back-pointer stored immediately before the code buffer
(`JitCode.h:32`), so the linker sets `codeStart = result + sizeof(JitCodeHeader)` and then
rounds it up to `CodeAlignment` (`Linker.cpp:51`). `CodeAlignment` is 16 on both x86-64
(`Assembler-x64.h:259`) and arm64 (`Architecture-arm64.h:555`), while the header is 8 bytes,
so the 8-byte header plus 8 bytes of alignment padding put the entry a constant `0x10` past
the chunk base — `LAND_OFF` in the tooling. The BTB therefore records the trainer's entry,
and after the training pages are freed the mispredicted branch resolves to
`codeBase + n*grid + 0x10` inside the wasm target that reused them.

The `len_trainer` → grid mapping is measured, and lives in `demo.js`'s
`TRAINER_GRID_BY_LEN` and in `common.sh` as `GRID_STRIDE_X86_64` / `GRID_STRIDE_AARCH64`.
Override it with `GRID_STRIDE=` in the environment (consumed by `inspect_reuse.sh`), `$grid`
passed to `wasm_layout.gdb`, or `--trainer-grid` passed to `demo.js`.

Spraying every trainer wastes dispatcher slots on landings that miss the pools. Once the
layout has been measured, `demo.js --pool-layout "first_pool_offset,pool_len,pool_interval,nr_pools"`
takes a compressed, periodic form of the `[POOL]` bounds (bytes relative to trainer 0;
`first_pool_offset` may be negative), maps each trainer's `i*grid` to
`[pool_base, pool_end)` membership, and routes only the pool-landing trainers through
`train_btb`, so every dispatcher slot trains on a gadget-reaching address. It is the
runtime counterpart of `coverage.py`'s trampoline scoring.

## Tools

### `make_target_wasm.py`: generate wasm modules

The single generator, selecting along three orthogonal axes:

- `--arch {x86_64, aarch64}` — gadget machine code + branch ISA
- `--encoding {f64, v128}` — pool slot size (8 vs 16 bytes; v128 reaches ~53% pool density
  against ~36% for f64, so ~47% more landing-zone bytes per footprint)
- `--layout {trampoline, lattice}` — how the pool becomes a landing zone (defaults per-arch:
  `x86_64`→`trampoline`, `aarch64`→`lattice`, but selectable)

`trampoline` emits N functions, each with one end-pool `[ gadget | jmp-rel* ]`, since x86
keeps one RIP-relative pool per function and dedups identical consts. `lattice` emits one
function whose flushed constant pool spills into islands tuned onto the grid, each a
`[ B B | gadget | B* ]` pool, since aarch64 flushes into islands and does not dedup. The
geometry knobs (`--nr-func`, `--nr-loads`, `--pad-sep`, `--pad-cross-page`, `--pool-repeat`,
`--block-repeat`, `--pad-start-phase`, `--pad-fill-tail`, `--gadget-off`) default from a
per-layout/encoding table. It emits the `.wat`, compiles it with wat2wasm, and writes a
JSON sidecar in one step (`--out-dir` / `--name` shared by all three artifacts;
`--no-compile` emits only the `.wat`):

```sh
python3 make_target_wasm.py --arch x86_64 --encoding v128 --out-dir wasm
# -> wasm/leak_snippet_x86_64_v128.{wat,wasm,sidecar.json}
```

The sidecar records the `Config`, the `wat2wasm` invocation, sha256+size of both artifacts,
and `gadget.first64` — the little-endian uint64 the verifier feeds to gdb as `$pat`. The
per-arch machine code lives in the strategy libraries `make_target_wasm_x86_64.py` /
`make_target_wasm_aarch64.py` (`assemble`, `build_gadget()`, `trampoline()`, `NOP`), which
the generator imports and which are not run directly. `tune_target.py` drives the generator
in a search loop, picking the `nr_loads` (coarse) and `pad_fill_tail` (fine) that make the
x86_64 target's JIT `codeLength` fill a whole number of 64 KB exec pages while keeping the
pool dense, landing in the window where `RandomPaddingForCodeLength` is forced to 0;
`--deploy` writes the winner into `wasm/`.

### `jit_ev.gdb`: inspect JIT compilation and executable memory allocation events

Traces every Baseline/Ion compilation (with the script filename) so you can see which
functions compile, and when. It also traces the raw executable-memory pool that JS JIT and
wasm share (`ProcessExecutableMemory`): `[EXEC ALLOC]`/`[EXEC FREE]` lines report every
64 KB page-level alloc/free with the resulting `cursor` and live page count, so the
first-fit-from-cursor reuse — freed training pages picked up by the next wasm target block —
can be watched directly alongside the per-code-block events.

### `wasm_layout.gdb`: inspect pool layout

Checks a loaded module against the sidecar its generator wrote, and reports where the JIT
actually placed it. It holds no pool geometry of its own: the pool's **shape** is declared
by the sidecar (`$pat`/`$slot`/`$nslot`/`$goff`, wired in by `inspect_reuse.sh`) and
verified here, while the pool's **position** is discovered at run time by searching for the
gadget. It breaks at `WasmJS.cpp:1655`, inside `WebAssembly.Module` construction, and reads
the Tier-1 `CodeBlock` layout by hand since the accessors are inlined away; the raw field
offsets are documented at the top of the script, and its driver is `probe_load.js`, which
instantiates the `.wasm` given as its argument. After dumping the module's code base/size
and per-function ranges it runs two passes:

- `[SEARCH]` enumerates **every** pool by scanning the whole code buffer for `$pat` — the
  invariant puts `$pat` `$goff` bytes into each pool and nowhere else, so each match is one
  pool's gadget. Each emits a machine-readable
  `[POOL] idx=N base=0x.. end=0x.. slots=.. last_ok=..` line: codeBase-relative bounds, with
  `base` the match minus `$goff`, `end` as `base + $nslot*$slot`, and the last slot checked
  to still be a trampoline. `last_ok=1` means the sidecar and the live code agree; a
  declaration that did not hold is reported `end=NONE`, and `coverage.py` then refuses to
  score. This finds the x86 target's per-function pools **and** every aarch64 lattice
  island. `$slot` (sidecar `gadget.bytes/gadget.chunks`), `$nslot` (sidecar
  `config.nr_loads`) and `$goff` (sidecar `config.gadget_off`, default `0`) describe the
  committed v128 build by default; `$nslot=0` disables the pass.
- `[GRID]` is a measurement, not a check, and the one pass that consults no part of the
  sidecar's pool description. It classifies every `codeBase + n*$grid + LAND_OFF` landing as
  trampoline→gadget / direct-gadget / miss purely by the bytes there, and prints the landing
  rate (45/86 for the default v128 target at `$grid=0x600`). `$grid` describes the machine,
  not the target.

The only arch-specific step, the branch decode, plus each arch's default `$pat` are inlined
and selected by `$arch` (default `x86_64`; x86-64 decodes `jmp rel32`, aarch64 decodes `B`).
Nothing external is sourced, so the probe runs the same from `tools/` or the parent dir.
`$grid` is required; `$pat` / `$arch` are overridable with `-ex` before `-x`:

```sh
gdb -q -nx -batch -ex 'set $grid=0x600' -x wasm_layout.gdb \
    --args "$JS_CMD" probe_load.js leak_snippet_x86_64_v128.wasm
```

### `coverage.py`: score trainer landings against the measured layout

Allocation-coverage post-processing for `inspect_reuse.sh`, one tool for both arches. It
scores each JS trainer chunk (from the log) against the live pool layout — a
`wasm_layout.gdb` `[POOL]` dump passed as `--layout` — point-based, under a `--mode`:

- `trampoline` (default): a trainer landing *anywhere* in a pool reaches the gadget via a
  trampoline, so it counts if its target-relative offset ∈ `[pool_base, pool_end)`.
- `gadget`: only a *direct* landing works, so it counts only if the offset `== pool_base`
  (slot 0).

No target geometry or trainer-size constant is hardcoded:

```sh
python tools/coverage.py <log> [demo.js] --layout layout.log [--mode gadget]
```

aarch64 needs nothing further than a committed gadget: the `[POOL]` pass enumerates all
lattice islands by whole-buffer search, so once the ARM target ships a real `$pat` it
produces ranges like x86_64 does today.

### `jit_churn.awk`: attribute mid-loop compilations to a `go()` rep

Shows where warmup is still leaking JIT compilations into the measurement loop. It reads
`demo.js` first to map each function's start line to its name, so it survives edits to the
driver, then scans a `jit_ev.gdb` log; `go()` prints `rep=i:` *after* finishing rep `i`, so
every compile between `rep=(i-1):` and `rep=i:` belongs to rep `i`. Dynamically spawned
trainers are the intended per-cycle allocations and are skipped.

```sh
awk -f tools/jit_churn.awk demo.js jit.log
```

### `parse_bailouts.py`: attribute Ion bailouts to a `go()` rep

The bailout counterpart to `jit_churn.awk`. It runs the driver under
`IONFLAGS=bailouts,bl-bails`, or re-parses a saved log, groups every Ion bailout by kind,
phase, and enclosing function, and attributes each to the `go()` iteration it fired in,
correcting for the same delayed `rep=i:` print. Bailouts past `--warmup`, and any
non-`FirstExecution` bailout inside the loop, are flagged `SUSPICIOUS` with a non-zero exit
status, so it drops into parameter sweeps. `--json` gives machine output.

```sh
JS_CMD=… tools/parse_bailouts.py -- <demo.js args>
tools/parse_bailouts.py --log merged.log
```

The two remaining drivers here are not standalone tools: `abi_check.js` backs
`abi_check.sh` (callee ABI recovered from `disnative` of the real `demo.js` callees) and
`fr_calib.js` backs `fr_calib.sh` (FLUSH+RELOAD hit/miss distributions, printing the
`--fr-threshold` this host supports).
