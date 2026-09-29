# Branch Target Reuse (BTR)
# April 30th 2026
# Yuhui Zhu

# Checks a compiled leak/target wasm module against the sidecar its generator
# wrote, and reports where the JIT actually placed it. This script contains no
# pool geometry of its own: the pool's SHAPE is declared by the sidecar and
# verified here, while the pool's POSITION is discovered at run time by searching
# the code buffer for the gadget. Two passes:
#   [SEARCH] enumerate EVERY literal pool by scanning the whole code buffer for
#            $pat. The [ trampoline* | gadget | trampoline* ] invariant
#            (make_target_wasm.py) puts $pat $goff bytes into every pool and
#            nowhere else, so each match is one pool's gadget and match - $goff is
#            that pool's slot 0. [POOL] then bounds each pool from the declared
#            $nslot/$slot and CHECKS the declaration: the last slot must still be
#            a trampoline to the gadget, else the pool is reported end=NONE and
#            tools/coverage.py refuses to score. Pool positions are never assumed,
#            so this finds the per-function pools of the x86 trampoline target AND
#            every island of the aarch64 lattice target alike.
#   [GRID]   measurement, not verification: walk every BTB landing point
#            (codeBase + n*$grid + LAND_OFF) and classify it by the bytes found
#            there -- a branch whose target is the gadget, a direct gadget
#            landing, or a miss. The good/total ratio is the structural
#            landing-rate for this template. The branch decode is the only
#            arch-specific step -- it is inlined below, selected by $arch.
#
# $pat / $slot / $nslot / $goff are the sidecar's CLAIM about the target, passed
# in with -ex before -x (inspect_reuse.sh extracts them); they are what [POOL]
# checks against the live code, not constants this script knows. $grid and $arch
# describe the machine instead, so they are not sidecar-derived.
#
# $pat  = the gadget's first 64 bits (little-endian uint64, sidecar
#         gadget.first64) -- the fingerprint [SEARCH] locates pools by. Each $arch
#         branch below supplies its committed-gadget default.
# $grid = BTB landing-grid stride in bytes (REQUIRED, no default). A uarch
#         property, not a target property -- [GRID] only.
# $arch = branch-decode ISA (x86_64 | aarch64); default x86_64.
# $slot = literal-pool slot size in bytes (sidecar gadget.bytes/gadget.chunks);
#         shared default 0x10 (v128, the committed build). Used by [POOL].
# $nslot= pool slots per pool (sidecar config.nr_loads); shared default 2448 (the
#         committed v128 build). Used by [POOL] to bound each pool from its
#         discovered start; 0 disables [POOL].
# $goff = the gadget's byte offset within every pool (sidecar config.gadget_off);
#         default 0, the x86 trampoline target. The aarch64 lattice target puts
#         the gadget at LAND_OFF so a grid landing hits it directly, and then
#         [POOL] reports pool bases $goff below the $pat matches.
#
# Usage (runs from tools/ OR the parent dir -- no arch module to source now;
# probe_load.js takes the .wasm path as its argument):
#   gdb -q -nx -batch -ex 'set $grid=0x600' -x tools/wasm_layout.gdb \
#       --args "$JS_CMD" tools/probe_load.js wasm/leak_snippet_x86_64_v128.wasm
#
# NB: WasmJS.cpp:1655 is inside WebAssembly.Module construction. The accessor
# methods (FuncToCodeRangeMap::operator[], CodeBlock::codeRange(), ...) are
# inlined away in this build, so we read the raw field layout by hand. These
# offsets were derived with `ptype /o` and are stable for Firefox 143:
#   CodeBlock+32  = codeBase (uint8_t*)
#   CodeBlock+40  = codeLength (size_t)
#   CodeBlock+48  = FuncToCodeRangeMap { startFuncIndex_@0, funcToCodeRange_@8 }
#     Uint32Vector { mBegin@0, mLength@8 }  => +56 = mBegin(u32*), +64 = mLength
#   CodeBlock+112 = CodeRangeVector { mBegin(CodeRange*)@0, mLength@8 }
#   sizeof(CodeRange) == 28; within it begin_@0(u32), ret_@4, end_@8(u32)

set pagination off
set confirm off

# $grid = BTB landing-grid stride (bytes); the mispredicted branch lands at
# codeBase + n*$grid. There is no safe default -- abort if the caller did not
# pass it via `-ex 'set $grid=...'`.
if $_isvoid($grid)
  printf "[ERROR] $grid not set -- pass the landing-grid stride, e.g. -ex 'set $grid=0x600'\n"
  quit 1
end

# $arch selects the arch-specific branch decode and default $pat. Defaults to
# x86_64; override with `-ex 'set $arch="aarch64"'` before -x. The arch specifics
# are inlined here (no external module to source), so the probe runs identically
# from tools/ or from the parent dir.
if $_isvoid($arch)
  set $arch = "x86_64"
end

# decode_tramp reads $_landing, $codebase, $codeend, $pat and sets $is_tramp = 1
# iff the landing is an unconditional branch whose in-block target's first 64 bits
# == $pat (a trampoline back to the gadget); the target is dereferenced only once
# proven in-block, so a wild disp can never fault the probe. It is defined ONCE for
# the selected $arch (so the hot [GRID] loop pays no per-call dispatch), and each
# branch also supplies that arch's committed-gadget default $pat, applied only if
# the caller did not pass -ex 'set $pat=...'. Each default is the first64 of the
# committed target's sidecar (wasm/leak_snippet_<arch>_*.sidecar.json).
if $_streq($arch, "aarch64")
  # `B` (bits[31:26]==0b000101, imm26); target = $_landing + sext(imm26)*4
  # (word-scaled, relative to the branch itself; `BL`=0b100101 is excluded).
  define decode_tramp
    set $is_tramp = 0
    set $insn = *(unsigned int*)$_landing
    if ($insn >> 26) == 0x05
      # (long) keeps the sign-extended imm26 in 64-bit signed arithmetic, so a
      # backward branch scales and adds to $_landing as a negative displacement.
      set $imm26 = (long)($insn & 0x3ffffff)
      if $imm26 & 0x2000000
        set $imm26 = $imm26 - 0x4000000
      end
      set $jmp_target = (char*)$_landing + $imm26 * 4
      if $jmp_target >= $codebase && $jmp_target < $codeend - 8
        if *(unsigned long*)$jmp_target == $pat
          set $is_tramp = 1
        end
      end
    end
  end
  if $_isvoid($pat)
    set $pat = 0xd24f3800f9400f80
  end
else
  # x86-64 `jmp rel32` (0xe9 + signed i32 disp); target = ($_landing+5)+disp.
  define decode_tramp
    set $is_tramp = 0
    if *(unsigned char*)$_landing == 0xe9
      set $disp = *(int*)((char*)$_landing + 1)
      set $jmp_target = (char*)$_landing + 5 + $disp
      if $jmp_target >= $codebase && $jmp_target < $codeend - 8
        if *(unsigned long*)$jmp_target == $pat
          set $is_tramp = 1
        end
      end
    end
  end
  if $_isvoid($pat)
    set $pat = 0x00ba482024448b48
  end
end

# $slot / $nslot / $goff are the sidecar's description of ONE pool -- slot size,
# slot count, and where the gadget sits inside it (gadget.bytes/gadget.chunks,
# config.nr_loads, config.gadget_off). [POOL] bounds each discovered pool from
# them and then checks the description holds. They are shared across arches; the
# defaults describe the committed v128 build. $nslot=0 disables [POOL].
if $_isvoid($slot)
  set $slot = 0x10
end
if $_isvoid($nslot)
  set $nslot = 2448
end
# $goff shifts a $pat match back to its pool: a match at address A belongs to the
# pool starting at A - $goff. 0 puts the gadget at slot 0 (the x86 trampoline
# target); the aarch64 lattice target declares LAND_OFF so a grid landing falls on
# the gadget's first instruction.
if $_isvoid($goff)
  set $goff = 0
end

# Given a pool's discovered slot-0 address $pstart and the sidecar's $slot/$nslot,
# set $pend (one past the pool) and $last_ok. The slot count is declared, so we
# never walk the pool: take the end as slot0 + $nslot*$slot, then CHECK that
# declaration against the code by confirming the LAST slot is still a trampoline
# to the gadget, per the [ trampoline* | gadget | trampoline* ] invariant (see
# header). $last_ok=1 iff the sidecar and the live code agree. decode_tramp
# recognizes the gadget by $pat, so this works for any $goff; it is the
# $arch-selected decoder defined above.
define pool_bounds
  set $pend = $pstart + $nslot * $slot
  set $last_ok = 0
  set $_landing = $pstart + ($nslot - 1) * $slot
  if $_landing + 8 < $codeend
    decode_tramp
    set $last_ok = $is_tramp
  end
end

# Transiently-redirected control flow lands at codeBase + n*$grid + LAND_OFF.
# LAND_OFF = 0x10 is the JS-JIT callee entry offset within its chunk, confirmed
# from Firefox source: each JIT code buffer is preceded by an 8-byte
# JitCodeHeader (jit/JitCode.h) and the entry is then aligned up to
# CodeAlignment=16 (jit/x64/Assembler-x64.h), so for a 16-aligned chunk base
# codeStart = base + 0x10 (jit/Linker.cpp: newCodeView). As long as $grid is
# 16-aligned every chunk base is 16-aligned, so this is stable at 0x10 (it
# would be 0x8 only for an 8-but-not-16-aligned base). LAND_OFF is a multiple
# of 8, so landings stay slot-aligned.
set $LAND_OFF = 0x10

b WasmJS.cpp:1655
commands
silent
set $cb = module.mRawPtr->code_.mRawPtr->completeTier1_
set $codebase = $cb->codeBase
set $codelength = $cb->codeLength
printf "[WASM] codeBase=%p codeLength=0x%lx\n", $codebase, $codelength

set $startFuncIndex = *(unsigned int*)((char*)$cb + 48)
set $ftcr_begin      = *(unsigned int **)((char*)$cb + 56)
set $ftcr_len         = *(unsigned long *)((char*)$cb + 64)
set $cr_begin         = *(char **)((char*)$cb + 112)
set $cr_len           = *(unsigned long *)((char*)$cb + 120)
printf "[WASM] startFuncIndex=%d funcToCodeRange.len=%lu codeRanges.len=%lu\n", \
       $startFuncIndex, $ftcr_len, $cr_len

printf "[WASM] per-function code ranges (note: end_ is end of INSTRUCTIONS;\n"
printf "       the literal pool lives between end_ and the next begin):\n"
set $i = 0
while $i < $ftcr_len
  set $cri = $ftcr_begin[$i]
  set $begin = *(unsigned int*)($cr_begin + $cri*28 + 0)
  set $end   = *(unsigned int*)($cr_begin + $cri*28 + 8)
  printf "  func[%d] crIdx=%u begin=0x%06x end=0x%06x insn_size=0x%x abs_begin=%p\n", \
         $i + $startFuncIndex, $cri, $begin, $end, $end - $begin, $codebase + $begin
  set $i = $i + 1
end

# Whole-buffer pool enumeration. The [ trampoline* | gadget | trampoline* ]
# invariant puts the gadget's first 64 bits ($pat) $goff bytes into EVERY pool and
# nowhere else (a trampoline's first bytes are a branch, not the gadget), so every
# $pat match is exactly one pool's gadget and slot 0 is $goff below it. We walk the
# whole code buffer, find each match, and advance the cursor past that pool. Pool
# positions come out of this search rather than from any assumption, so it
# enumerates the x86 trampoline target's per-function pools AND all the aarch64
# lattice islands without knowing which layout produced them. Each match is bounded
# and checked by pool_bounds and emitted as a machine-readable [POOL] line for
# tools/coverage.py ($nslot=0 suppresses [POOL]). A pool whose declaration did not
# hold (the last slot is not a trampoline to the gadget, i.e. the sidecar's
# $nslot/$slot/$goff describe something other than the module that was loaded) is
# reported end=NONE so the consumer aborts rather than trust a wrong range; if the
# gadget is never found the absence of any [POOL] line makes coverage abort too.
printf "\n[SEARCH] pattern=0x%016lx (whole-buffer pool enumeration)\n", $pat
set $codeend = (char*)$codebase + $codelength
set $idx = 0
set $cursor = (char*)$codebase
set $more = 1
while $more
  if $cursor > $codeend - 8
    set $more = 0
  else
    find /1 $cursor, $codeend - 8, (unsigned long)$pat
    if $numfound > 0
      set $hit = (char*)$_
      set $pstart = $hit - $goff
      printf "  pool[%d] slot0 @ %p -> codeBase+0x%lx (gadget @ +0x%x)\n", \
             $idx, $pstart, (long)($pstart - (char*)$codebase), $goff
      if $nslot > 0
        pool_bounds
        if $last_ok
          printf "[POOL] idx=%d base=0x%lx end=0x%lx slots=%ld last_ok=1\n", \
                 $idx, (long)($pstart - (char*)$codebase), \
                 (long)($pend - (char*)$codebase), $nslot
          set $cursor = $pend
        else
          printf "[POOL] idx=%d base=0x%lx end=NONE slots=%ld last_ok=0\n", \
                 $idx, (long)($pstart - (char*)$codebase), $nslot
          set $cursor = $hit + $slot
        end
      else
        set $cursor = $hit + 8
      end
      set $idx = $idx + 1
    else
      set $more = 0
    end
  end
end
if $idx == 0
  printf "  pattern NOT FOUND anywhere in the code buffer\n"
end

# The JIT hands out code from 64KB executable pools (ExecutableCodePageSize,
# js/src/jit/ProcessExecutableMemory.h:63) and bump-allocates within one
# (ExecutablePool::alloc, js/src/jit/ExecutableAllocator.cpp:71). A chunk that no
# longer fits opens a FRESH pool at the next pool boundary
# (ProcessExecutableMemory.cpp:833, base_ + page*ExecutableCodePageSize). So chunk
# bases march by $grid inside a pool and then RESTART at that boundary rather than
# continuing the progression -- the landing grid re-aligns on every crossing. The
# walk below models this with two cursors: $_page_cursor steps pool to pool and
# $_cursor steps by $grid inside the current pool.
#
# The pool boundaries are taken to start at codeBase. The real ones sit at
# base_ + k*ExecutableCodePageSize, and base_ is only page-aligned
# (ProcessExecutableMemory.cpp:693-696 says it is "NOT guaranteed to be aligned to
# ExecutableCodePageSize"), so codeBase carries an unknown 4KB-granular phase
# within its pool. Fixing the phase at codeBase makes the rate a structural
# property of the target rather than of one run's address randomization.
set $POOL_SIZE = 0x10000

# Grid-landing classification -- a measurement, and the one pass that consults no
# part of the sidecar's pool description: it walks every BTB landing point
# (poolBase + n*$grid + LAND_OFF, the phase resetting per the pool re-alignment
# above) and classifies it purely by the bytes there, using $pat as the gadget
# fingerprint:
#   TRAMP  : decode_tramp (the $arch-selected decoder) says the landing is a
#            branch whose in-block target's first 64 bits == $pat -- a mispredict
#            here transfers to the gadget                             (GOOD)
#   GADGET : the landing's own first 64 bits == $pat -- it fell on the gadget
#            directly                                                 (GOOD)
#   MISS   : anything else -- does not reach the gadget.
# Only the branch decode is arch-specific (inlined above, selected by $arch); the
# walk, the $pat compare, and the summary are shared. Both good outcomes are
# counted, so a target whose gadget sits at $goff scores the same whether the
# landing hits the gadget itself or a trampoline ahead of it.
# "reach the gadget" = TRAMP + GADGET.
printf "\n[GRID] classifying landings at poolBase + n*0x%lx + 0x%x ", $grid, $LAND_OFF
printf "(arch=%s, pool 0x%lx)\n", $arch, $POOL_SIZE
set $cnt_tramp  = 0
set $cnt_gadget = 0
set $cnt_miss   = 0
set $n = 0
set $nreset = 0
set $_cursor = $codebase
set $_page_cursor = $codebase
while $_cursor + $LAND_OFF < $codeend
  set $_landing = $_cursor + $LAND_OFF
  decode_tramp
  if $is_tramp
    set $cnt_tramp = $cnt_tramp + 1
  else
    if *(unsigned long*)$_landing == $pat
      set $cnt_gadget = $cnt_gadget + 1
    else
      set $cnt_miss = $cnt_miss + 1
    end
  end
  set $n = $n + 1
  set $_cursor = $_cursor + $grid
  # Stepping out of the current pool means that chunk no longer fits: it opens
  # the next pool and is placed at its base, so the grid phase resets there.
  if $_cursor >= $_page_cursor + $POOL_SIZE
    set $_page_cursor = $_page_cursor + $POOL_SIZE
    set $_cursor = $_page_cursor
    set $nreset = $nreset + 1
  end
end

printf "\n[GRID SUMMARY] %d grid points (stride 0x%lx, phase reset at %d 0x%lx ", \
       $n, $grid, $nreset, $POOL_SIZE
printf "pool boundaries) over codeLength 0x%lx:\n", $codelength
printf "  jmp-rel32 -> gadget    (GOOD, trampoline)  : %d\n", $cnt_tramp
printf "  landed on gadget slot0 (GOOD, full gadget) : %d\n", $cnt_gadget
printf "  other bytes            (does not reach it) : %d\n", $cnt_miss
set $good = $cnt_tramp + $cnt_gadget
printf "  => %d/%d grid points reach the gadget\n", $good, $n

continue
end

run
exit
