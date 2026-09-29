set pagination off
set confirm off

# Baseline JIT compilation (JS)
b BaselineCodeGen.cpp:333
commands
silent
set $script_filename = (char*)(**(ScriptSource**)((void*)script.ptr.sourceObject_.value + sizeof(NativeObject) + (js::ScriptSourceObject::SOURCE_SLOT)*sizeof(HeapSlot*))).filename_.box_.chars_
set $script_lineno = script.ptr.extent_.lineno
printf "[BASELINE] %s:%d  code=%p  size=%p\n", $script_filename, $script_lineno, code->header_.value_, code->bufferSize_
continue
end

# IonMonkey tier-up (JS)
b CodeGenerator.cpp:17271
commands
silent
set $code = ionScript->method_.value
set $script_filename = (char*)(**(ScriptSource**)((void*)script.ptr.sourceObject_.value + sizeof(NativeObject) + (js::ScriptSourceObject::SOURCE_SLOT)*sizeof(HeapSlot*))).filename_.box_.chars_
set $script_lineno = script.ptr.extent_.lineno
printf "[TIER-UP] %s:%d  code=%p  size=%p\n", $script_filename, $script_lineno, $code->header_.value_, $code->insnSize_
continue
end

# Wasm compilation (per code block / per tier)
# Code::addCodeBlock() calls CodeBlock::initialize() once for every finished
# code block -- shared stubs, the baseline tier, the optimized (Ion) tier on
# tier-up, and lazy stubs. Unlike the compile-task/finishCodeBlock bodies
# (which are inlined and cannot be evaluated inside their inlined frames),
# CodeBlock::initialize is a real out-of-line member function, so `this` and
# its fields are readable here. By the time it runs, finishCodeBlock has
# already populated this->codeBase / codeLength / segment, and this->kind
# gives the tier -- no deep module->code->completeTier1 pointer chasing.
# Breakpoint is by symbol (not file:line) so it survives source line drift.
b js::wasm::CodeBlock::initialize
commands
silent
set $kind = (int)this->kind
if $kind == 0
    printf "[WASM STUBS]     code=%p  size=%p\n", this->codeBase, this->codeLength
end
if $kind == 1
    printf "[WASM BASELINE]  code=%p  size=%p\n", this->codeBase, this->codeLength
end
if $kind == 2
    printf "[WASM ION]       code=%p  size=%p\n", this->codeBase, this->codeLength
end
if $kind == 3
    printf "[WASM LAZYSTUB]  code=%p  size=%p\n", this->codeBase, this->codeLength
end
continue
end

# Extra executable memory allocation hooks begin
# Enable following hooks with extra gdb argument `--ex 'set $full_log=1'`
if !$_isvoid($full_log)

# Raw executable-memory page allocator (the pool JS JIT AND wasm both draw from)
# --------------------------------------------------------------------------
# js::jit::AllocateExecutableMemory -> ProcessExecutableMemory::allocate hands
# out ExecutableCodePageSize (= 64 KiB = 0x10000) pages by first-fit from
# cursor_; deallocate() rewinds cursor_ to the lowest freed page, which is what
# lets a freed JS-training run be reused by the next wasm target block. These
# two hooks trace every page-level alloc/free with the resulting cursor and
# live page count, so the reuse can be watched directly (below the per-code-block
# events above). `live` is the total pages held after the event; `cursor` is
# where the next allocation's first-fit scan starts.
#
# ALLOC is a file:line breakpoint (not a symbol) because the success point is
# mid-function: the returned pointer is optimized out by the tail, but at the
# `p = base_ + page * ExecutableCodePageSize` line `page` is still live, so the
# base is reconstructed as base_ + page*0x10000. cursor_ is already advanced
# here (for numPages<=2) and pagesAllocated_ already incremented. If the source
# drifts, re-anchor the line onto that `p = base_ + page * ...` statement.
b ProcessExecutableMemory.cpp:833
commands
silent
printf "[EXEC ALLOC] addr=%p  size=%p  pages=%lu  cursor=%p(pg %lu)  live=%lu\n", (void*)(this->base_ + page*0x10000), bytes, (unsigned long)numPages, (void*)(this->base_ + this->cursor_*0x10000), (unsigned long)this->cursor_, (unsigned long)this->pagesAllocated_
continue
end

# FREE hooks the public DeallocateExecutableMemory symbol (survives line drift);
# its addr/bytes args are clean, whereas the member deallocate's first param is
# folded to an unusable name. The rewind hasn't run yet at this point, so the
# post-free cursor is computed the same way deallocate() will: min(cursor_,
# firstPage). ($firstpg/$newcur are named to avoid gdb's $fp/$sp/$pc registers.)
b js::jit::DeallocateExecutableMemory
commands
silent
set $firstpg = ((unsigned long)addr - (unsigned long)execMemory.base_)/0x10000
set $newcur = (unsigned long)execMemory.cursor_
if $firstpg < $newcur
set $newcur = $firstpg
end
printf "[EXEC FREE ] addr=%p  size=%p  pages=%lu  cursor=%p(pg %lu)  live=%lu\n", addr, bytes, (unsigned long)(bytes/0x10000), (void*)(execMemory.base_ + $newcur*0x10000), $newcur, (unsigned long)execMemory.pagesAllocated_ - (unsigned long)(bytes/0x10000)
continue
end

# Per-JitCode sub-allocation classified by CodeKind and owning pool.
# --------------------------------------------------------------------------
# The page-level EXEC ALLOC/FREE hooks above see only the 64 KB pool pages.
# But JS JIT does NOT free a page per JitCode: ExecutableAllocator::alloc packs
# each JitCode into a refcounted ExecutablePool (small allocs <=64KB share one
# of m_smallPools), and the page returns to ProcessExecutableMemory only when
# that pool's m_refCount hits 0. So one long-lived JitCode -- a JitRuntime
# trampoline (Other), a Baseline/Ion IC stub, or a RegExp -- pins a whole page
# even after the baseline/Ion script that shared it is discarded. This hook
# classifies every sub-alloc so you can see WHICH kind lands in WHICH pool.
#
# Broken at ExecutableAllocator.cpp:216 (not the entry): *poolp is filled by
# poolForSize() at line 208 and `result` is set at 215, so at 216 all of
# result / *poolp / n / type are live. poolbase == (*poolp)->m_allocation.pages
# is the 64 KB page reported by [EXEC ALLOC] when that pool was created, so the
# two traces join on poolbase. refcnt is how many live JitCodes share the page.
# kind: 0=Ion  1=Baseline  2=RegExp  3=Other
b ExecutableAllocator.cpp:216
commands
silent
printf "[EXEC SUBALLOC] kind=%d  n=%p  addr=%p  poolbase=%p  poolsize=%p  refcnt=%u\n", (int)type, (void*)n, result, (*poolp)->m_allocation.pages, (void*)(*poolp)->m_allocation.size, (unsigned)(*poolp)->m_refCount
continue
end

# Pool actually returning its pages (m_refCount reached 0). This is the JS-side
# counterpart to [EXEC FREE]: releasePoolPages -> systemRelease -> the page-level
# free fires right after. When a page lingers, the last [EXEC SUBALLOC] with this
# poolbase before this line tells you which kind kept it alive; if this line
# never fires for a poolbase, some JitCode in it is permanently live (trampolines
# / baseline interpreter / a surviving IC stub or RegExp).
b js::jit::ExecutableAllocator::releasePoolPages
commands
silent
printf "[EXEC POOLFREE] poolbase=%p  poolsize=%p\n", pool->m_allocation.pages, (void*)pool->m_allocation.size
continue
end

end
# Extra executable memory allocation hooks end

run