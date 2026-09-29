#define _GNU_SOURCE
#include <sys/mman.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <sched.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <string.h>
#include <sys/time.h>
#include <math.h>
#include <stdbool.h>
#include <signal.h>
#include <pthread.h>
#include <sys/wait.h>

#include "config.h"
#include "helper.h"

extern char reload_instruction[];

#if ARCH == X86
void rsb_overwrite_target() {
    // "movq   (%%rsi), %%rax\n\t"
    reload_instruction[2] = 0x6;
    fence();
}

void rsb_restore_target(){
    // "movq   (%%rdi), %%rax\n\t"
    reload_instruction[2] = 0x7;
    fence();
}

__attribute__((__noinline__, noclone)) static void rsb_caller0(uint8_t * fr_buf1, uint8_t * fr_buf2){

	asm volatile (
		"call overwrite_return\n\t"
		"/* Transient window starts here */\n\t"
        ".global reload_instruction\n\t"
        "reload_instruction:\n\t"
		"movq   (%%rdi), %%rax\n\t"

		"infinite_loop:\n\t"
			"pause\n\t"
			"jmp infinite_loop\n\t"
	    "overwrite_return:\n\t"
            "call rsb_overwrite_target\n\t"
			"leaq    12(%%rip), %%r11\n\t"
			"movq    %%r11, (%%rsp)\n\t"
			"clflush (%%rsp)\n\t"
			"mfence\n\t"
			"ret\n\t"

		"arch_ret:\n\t"
			:
		: "D" (fr_buf1), "S" (fr_buf2)
		: "%rax", "%r11", "memory"
		);

    return;

}

#elif ARCH == AARCH64

void rsb_overwrite_target(void) {
    // AArch64: Instructions are fixed 32-bit.
    // fr_buf1 is pinned to x19, fr_buf2 is pinned to x20 (callee-saved,
    // so they survive the nested call below).
    // ldrb w0, [x20] encoding is 0x38400000 | (20 << 5)
    uint32_t *inst_ptr = (uint32_t *)reload_instruction;
    *inst_ptr = 0x38400000 | (20 << 5);

    // Data Synchronization Barrier & Instruction Synchronization Barrier
    asm volatile("dsb sy\n\tisb" ::: "memory");

    // CRITICAL: You must also clear the cache for the modified reload_instruction on ARM
    __builtin___clear_cache((char*)inst_ptr, (char*)inst_ptr + 4);
}

void rsb_restore_target(void) {
    // AArch64: ldrb w0, [x19] encoding is 0x38400000 | (19 << 5)
    uint32_t *inst_ptr = (uint32_t *)reload_instruction;
    *inst_ptr = 0x38400000 | (19 << 5);

    // Data Synchronization Barrier & Instruction Synchronization Barrier
    asm volatile("dsb sy\n\tisb" ::: "memory");

    // Clear cache for the restored instruction
    __builtin___clear_cache((char*)inst_ptr, (char*)inst_ptr + 4);
}

__attribute__((__noinline__, noclone)) void rsb_caller0(uint8_t * fr_buf1, uint8_t * fr_buf2){
    // ARM64 Equivalent
    register uint8_t * fb1_x19 asm("x19") = fr_buf1;
    register uint8_t * fb2_x20 asm("x20") = fr_buf2;
    asm volatile (
        "bl overwrite_return\n\t"
        "/* Transient window starts here */\n\t"
        ".global reload_instruction\n\t"
        "reload_instruction:\n\t"
        "ldrb w0, [x19]\n\t"

        "infinite_loop:\n\t"
            "yield\n\t"
            "b infinite_loop\n\t"
        "overwrite_return:\n\t"
            "str x30, [sp, #-16]!\n\t" // Save Link Register
            "bl rsb_overwrite_target\n\t"

            // Calculate new return address (arch_ret)
            "adr x11, arch_ret\n\t"

            // Overwrite the saved link register on the stack
            "mov x30, x11\n\t"

            "ret\n\t"         // Speculatively returns to reload_instruction, architecturally to arch_ret
        "arch_ret:\n\t"
            "ldr x30, [sp], #16\n\t" // Restore clean state if needed
            :
        : "r" (fb1_x19), "r" (fb2_x20)
        : "x0", "x11", "x30", "memory"
    );
}

#else
    #error "Architecture not supported."
#endif

static uint64_t rsb_do_flush_and_reload(struct config * cfg, uint64_t hits[2], uint64_t iterations, int fr_idx) {

    set_ibpb(cfg->cpu_nr);

    // Avoid constant propagation in caller0 function
    volatile int idx = 0;

    // do FR_BUF_SEQUENTIAL_TESTING by default
    for (int fr_idx = 0; fr_idx < 2; fr_idx++) {

        for(int outer=0; outer < iterations; outer++) {
                flush(cfg->fr_buf[fr_idx]);

                rsb_caller0(cfg->fr_buf[0], cfg->fr_buf[1]);
                fence();

                rsb_restore_target();

                if(load_time(cfg->fr_buf[fr_idx]) < THR) {
                    hits[fr_idx]++;
                }
        }
    }

    return 0;
}

void do_rsb_experiment(struct config * cfg) {

    uint64_t hits[2], iter;
    uint64_t iterations = 10000;
    long page_size = sysconf(_SC_PAGESIZE);
    void * page_base = (void *) ((uint64_t) rsb_caller0 & ~((uint64_t) page_size - 1));
    assert(mprotect(page_base, page_size, PROT_EXEC|PROT_READ|PROT_WRITE) == 0);

    printf("RSB: Hits? -> 'Stale' RSB reuse\n");

    for (size_t i = 0; i < 2; i++)
    {
        for (int i = 0; i < 2; i++) { hits[i] = 0; }
        rsb_do_flush_and_reload(cfg, hits, 10000, 0);

        printf("RBB_TEST. Total hits: %5ld, fr_buf[0]: %5ld, fr_buf[1]: %5ld, / %ld\n", hits[0] + hits[1], hits[0], hits[1], iterations);
        fflush(stdout);

    }

    return;
}
