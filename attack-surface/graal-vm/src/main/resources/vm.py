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
got_args = False

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
        else:
            i += 1

init_dummy_callee()

def main_loop():
    global global_counter
    global target_prog_template
    global tenured_progs
    global got_args

    if not got_args:
        parse_args()
        print("Config: nr_bh={}, nr_bcond={}, nr_bcond_taken={}, nr_trainer={}, len_trainer={}, nr_target={}, len_target={}, skip_reuse={}".format(
            nr_bh, nr_bcond, nr_bcond_taken, nr_trainer, len_trainer, nr_target, len_target, flag_skip_reuse
        ))
        print("train_type={}, loop_type={}, do_overwrite={}".format(
            _cfg.train_type, _cfg.loop_type, _cfg.do_overwrite
        ))
        update_config(nr_bh, nr_bcond, nr_bcond_taken)
        init_buffers()
        init_all_rand_bh()
        got_args = True

    global_counter += 1
    # if target_prog_template is None:
    #     init_target_prog_template(len_target)
    
    # flag_stage_init = int(global_counter<30)    
    # nr_train_padding = (10 & (flag_stage_init - 1)) + (2 & flag_stage_init<<1)
    # target_nr = (target_nr & (flag_stage_init - 1)) + (2 & flag_stage_init<<1)

    # Trigger GC to reclaim target chunks
    gc_mod = compile(TRAIN_PROG_TEMPLATE.format(0), "gc0_{}".format(global_counter), "exec")
    gc_globals = {}
    exec(gc_mod, gc_globals)
    for i in range(50):
        gc_globals["func0"]()

    time.sleep(0.05)
    gc_mod = None
    gc_globals.clear()
    gc.collect()
    time.sleep(0.05)

    # Trigger compile for training chunks
    for _ in range(nr_target):
        train_mod = compile(TRAIN_PROG_TEMPLATE.format(0), "train_{}_{}".format(global_counter, _), "exec")
        train_globals = {}
        exec(train_mod, train_globals)
        for i in range(1000):
            train_globals["func0"]()
        time.sleep(0.01)

    # Simulate training iBTB
    hits = (ctypes.c_uint64 * 2)(0, 0)
    _clib.do_train(ctypes.byref(_cfg), 1)

    if not flag_skip_reuse:
        gc.collect()   
        time.sleep(0.05)  

        # Trigger GC to reclaim training chunks
        gc_mod = compile(TRAIN_PROG_TEMPLATE.format(0), "gc1_{}".format(global_counter), "exec")
        gc_globals = {}
        exec(gc_mod, gc_globals)
        for i in range(50):
            gc_globals["func0"]()

        time.sleep(0.05)
        gc_mod = None
        gc_globals.clear()
        gc.collect()
        time.sleep(0.05)

        # Trigger compile for target chunks
        target_globals = {}
        for i in range(nr_target):
            target_mod = compile(TARGET_PROG_FRAG.format(0), "target_{}_{}".format(global_counter, i), "exec", optimize=2)
            exec(target_mod, target_globals)
            for _ in range(1000):
                target_globals["func0"]([0, 0, 0, 0])
            time.sleep(0.01)

        for _ in range(nr_bh):
            dummy_callee()

    # Mis-speculate to test survive iBTB entries
    _clib.do_reload(ctypes.byref(_cfg), hits, 0)
    print(hits[0], hits[1])

    gc.collect()
    return [int(hits[0]), int(hits[1])]