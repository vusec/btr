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


#include "common.h"
#include "config.h"
#include "helper.h"
#include "test_rsb.h"

#define STR(x) #x
#define XSTR(s) STR(s)

enum loopType {
  LOOP_NONE,
  LOOP_SLEEP,
  LOOP_BUSY,
  LOOP_BUSY_FORK
};

enum trainType {
  TRAIN_SINGLE_TARGET,
  TRAIN_MOCK_TARGET,
  TRAIN_ALTERNATE_TARGET,
};


int test_n_histories[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
int test_history_sizes[] = {64, 128, 256, 512};
int test_his_taken_branches[] = {16, 32, 48, 64, 96, 128, 192, 256};


#define MILLION 1000 * 1000
size_t test_busy_loop[] = {1 * MILLION,  10 * MILLION, 100 * MILLION, 1000 * MILLION};
size_t test_n_forks[] = {1, 10, 100, 1000};
size_t test_usleep[] = {100 * 1, 1000 * 1, 1000 * 10};

// ------------------------------------------------------------------------------------
// Targets

static __attribute__((__noinline__)) uint64_t target0(uint8_t * arg0, uint8_t * arg1){
    fence();
    return 1;
}

static __attribute__((__noinline__)) uint64_t f_target1(uint8_t * arg0, uint8_t * arg1){
    uint64_t ret = *(volatile uint8_t *)(arg0);
    fence();
}
static void f_target1_end(void *) {};

static __attribute__((__noinline__)) uint64_t f_target2(uint8_t * arg0, uint8_t * arg1){
    uint64_t ret = *(volatile uint8_t *)(arg1);
    fence();
}
static void f_target2_end(void *) {};


typedef uint64_t target_t(uint8_t * arg0, uint8_t * arg1);
target_t *ftable[] = {target0, NULL, NULL};

#define REPEAT_4(X)  X X X X
#define REPEAT_16(X) REPEAT_4(REPEAT_4(X))
#define REPEAT_64(X) REPEAT_4(REPEAT_16(X))
#define REPEAT_128(X) REPEAT_64(X) REPEAT_64(X)
#define REPEAT_256(X) REPEAT_4(REPEAT_64(X))

#define ONE_BRANCH \
    do { \
        if (*(volatile uint8_t *)history == 1) { asm volatile("" ::: "memory"); } \
        history += 1; \
    } while(0);

__attribute__((__noinline__, noclone)) void caller0(uint8_t * fr_buf1, uint8_t * fr_buf2, uint8_t * history, const volatile int idx, struct config * cfg){

    if (cfg->history_size <= 64) {
        goto his_64;

    } else if (cfg->history_size <= 128) {
        goto his_128;

    } else if (cfg->history_size <= 256) {
        goto his_256;

    } else  {
    }
    // return;
    REPEAT_256(ONE_BRANCH);
    REPEAT_256(ONE_BRANCH);
    fence();
    ftable[idx](fr_buf1, fr_buf2);
    return;

his_256:
    REPEAT_256(ONE_BRANCH);
    fence();
    ftable[idx](fr_buf1, fr_buf2);
    return;

his_128:
    fence();
    REPEAT_128(ONE_BRANCH);
    ftable[idx](fr_buf1, fr_buf2);
    return;

his_64:
    REPEAT_64(ONE_BRANCH);
    fence();
    ftable[idx](fr_buf1, fr_buf2);
    return;

}

// ------------------------------------------------------------------------------------


void overwrite_target() {
    memcpy(ftable[2], f_target1, (void *) f_target1_end - (void *) f_target1);
    memcpy(ftable[1], f_target2, (void *) f_target2_end - (void *) f_target2);

#if ARCH == AARCH64
    __builtin___clear_cache(ftable[2], ftable[2] + (uint64_t) f_target1_end - (uint64_t) f_target1);
    __builtin___clear_cache(ftable[1], ftable[1] + (uint64_t) f_target2_end - (uint64_t) f_target2);
#endif
}

void restore_target(){
    memcpy(ftable[1], f_target1, (void *) f_target1_end - (void *) f_target1);
    memcpy(ftable[2], f_target2, (void *) f_target2_end - (void *) f_target2);

#if ARCH == AARCH64
    __builtin___clear_cache(ftable[2], ftable[2] + (uint64_t) f_target1_end - (uint64_t) f_target1);
    __builtin___clear_cache(ftable[1], ftable[1] + (uint64_t) f_target2_end - (uint64_t) f_target2);
#endif
}


void do_sleep(struct config * cfg) {
    usleep(cfg->usleep);
}

void do_busy_loop(struct config * cfg) {
    uint64_t delta;

#if ARCH == X86
    uint64_t r0 = rdtscp();
#elif ARCH == AARCH64
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
#endif

    while (1)
    {

#if ARCH == X86
        delta = rdtscp() - r0;
        if (delta > cfg->busy_cycles) {
            break;
        }
#elif ARCH == AARCH64
        clock_gettime(CLOCK_MONOTONIC, &t1);
        uint64_t delta = (t1.tv_sec - t0.tv_sec) * 1000000 + (t1.tv_nsec - t0.tv_nsec);

        if (delta > (cfg->busy_cycles / 100000)) {
            break;
        }
#endif

    }

    return;
}

void do_busy_loop_fork(struct config * cfg) {
    uint64_t r0, delta_cycles;
    pid_t p;

    for (volatile int i = 0; i < cfg->n_forks; i++)
    {
        p = fork();
        if (p < 0) {
            perror("fork fail");
            exit(1);
        } else if (p == 0) {
            char* argv[] = { NULL };
            char* envp[] = { NULL };
            if (execve("/bin/true", argv, envp) == -1) {
                perror("Error during execve");
                exit(0);
            }
        }
        waitpid(p, NULL, 0);
    }

    return;
}

size_t get_busy_loop_cycles(size_t arg_usleep) {
    uint64_t delta_cycles, r0;

    for (size_t i = 0; i < 10; i++)
    {
        r0 = rdtscp();
        usleep(arg_usleep);

        delta_cycles += rdtscp() - r0;
    }
    return delta_cycles / 10;
}

size_t get_cycles_for_forks(size_t n_forks) {
    uint64_t r0, delta_cycles = 0;
    pid_t p;
    int n = 0;

    for (size_t iter = 0; iter < 10; iter++)
    {
        r0 = rdtscp();

        for (volatile int n = 0; n < n_forks; n++)
        {
            p = fork();
            if (p < 0) {
                perror("fork fail");
                exit(1);
            } else if (p == 0) {
                char* argv[] = { NULL };
                char* envp[] = { NULL };
                if (execve("/bin/true", argv, envp) == -1) {
                    perror("Error during execve");
                    exit(0);
                }
            }
            waitpid(p, NULL, 0);
        }

        delta_cycles += rdtscp() - r0;

    }

    return delta_cycles / 10;
}


void do_train(struct config * cfg, volatile int idx) {

    for (int outer = 0; outer < 2; outer++)
    {

        if (cfg->train_type == TRAIN_SINGLE_TARGET)
        {
            for (int his = 0; his < cfg->n_histories; his++)
            {
                for (size_t inner = 0; inner < 10; inner++) {
                    caller0(cfg->fr_buf[0], cfg->fr_buf[1], cfg->history[his], idx, cfg);
                    cpuid();
                }
            }
        } else if (cfg->train_type == TRAIN_MOCK_TARGET)
        {

            for (int his = 0; his < cfg->n_histories; his++)
            {
                for (size_t inner = 0; inner < 10; inner++) {
                    caller0(cfg->fr_buf[0], cfg->fr_buf[1], cfg->history[0], 0, cfg);
                    cpuid();
                }

                for (size_t inner = 0; inner < 10; inner++) {
                    caller0(cfg->fr_buf[0], cfg->fr_buf[1], cfg->history[his], idx, cfg);
                    cpuid();
                }
            }

        } else if (cfg->train_type == TRAIN_ALTERNATE_TARGET)
        {
            for (int his = 0; his < cfg->n_histories; his++)
            {
                for (size_t inner = 0; inner < 20; inner++) {
                    caller0(cfg->fr_buf[0], cfg->fr_buf[1], cfg->history[his], idx + (his % 2), cfg);
                    cpuid();
                }
            }
        }
    }

}
uint64_t do_reload(struct config * cfg, uint64_t hits[2], uint64_t iterations, int fr_idx) {
    int idx = 0;

    for (int his = 0; his < cfg->n_histories; his++)
    {

        fence();
        caller0(cfg->fr_buf[0], cfg->fr_buf[1], cfg->history[0], idx, cfg);

        fence();
        flush(&ftable[0]);

#ifdef  FR_BUF_SEQUENTIAL_TESTING
        flush(cfg->fr_buf[fr_idx]);
#else
        for (int i = 0; i < N_FR_BUF; i++) { flush(cfg->fr_buf[i]);}
#endif
        fence();


        caller0(cfg->fr_buf[0], cfg->fr_buf[1], cfg->history[his], idx, cfg);
        fence();


#ifdef  FR_BUF_SEQUENTIAL_TESTING
        if(load_time(cfg->fr_buf[fr_idx]) < THR) {
            hits[fr_idx]++;
        }
#else
        for (int i = 0; i < 2; i++) {
            if(load_time(cfg->fr_buf[i]) < THR) {
                hits[i]++;
            }
        }
#endif
    }
}

static uint64_t do_flush_and_reload(struct config * cfg, uint64_t hits[2], uint64_t iterations, int fr_idx) {

    set_ibpb(cfg->cpu_nr);

    // Avoid constant propagation in caller0 function
    volatile int idx = 0;

    for(int outer=0; outer < iterations; outer++) {

            idx = 1;

            do_train(cfg, idx);

            if (cfg->do_overwrite) {
                overwrite_target();
            }

            // do stuff
            // usleep(1000000 / 1000);
            if (cfg->loop_type == LOOP_SLEEP) {
                do_sleep(cfg);
            } else if (cfg->loop_type == LOOP_BUSY) {
                do_busy_loop(cfg);
            } else if (cfg->loop_type == LOOP_BUSY_FORK) {
                do_busy_loop_fork(cfg);
            }

            do_reload(cfg, hits, iterations, fr_idx);

            if (cfg->do_overwrite) {
                restore_target();
            }
    }

    return 0;
}


#define EXPERIMENT_ITERATIONS 2

uint64_t do_experiment(struct config * cfg) {
    uint64_t hits[2], iter, total_hits;

    for (size_t i = 0; i < cfg->n_histories; i++) {
        randomize_history(cfg, cfg->history[i]);
    }

    uint64_t loop_var = 0;

    if (cfg->loop_type == LOOP_SLEEP) {
        loop_var = cfg->usleep;
    } else if (cfg->loop_type == LOOP_BUSY) {
        loop_var = cfg->busy_cycles;
    } else if (cfg->loop_type == LOOP_BUSY_FORK) {
        loop_var = cfg->n_forks;
    }


    total_hits = 0;
    uint64_t iterations = 10;

    for (size_t i = 0; i < EXPERIMENT_ITERATIONS; i++)
    {
        for (int i = 0; i < 2; i++) { hits[i] = 0; }
        do_flush_and_reload(cfg, hits, iterations, 0);
#ifdef FR_BUF_SEQUENTIAL_TESTING
        do_flush_and_reload(cfg, hits, iterations, 1);
#endif
        printf("{'m': '%s', 'loop_type': %d, 'loop_var': %6ld, 'train_type': %d, 'overwrite': %d,'his_size': %3d, 'his_takes': %3d, 'n_his': %4d, 'hits': %5.1f} / %4d (%5.1f%%) | %5.1f %5.1f\n",
            ARCH_S, cfg->loop_type, loop_var, cfg->train_type, cfg->do_overwrite, cfg->history_size, cfg->his_taken_branches, cfg->n_histories,
            (double) (hits[0] + hits[1]) / iterations, cfg->n_histories, (double) ((double) (hits[0] + hits[1]) / iterations) / (double) cfg->n_histories * 100, (double) hits[0] / iterations, (double) hits[1] / iterations);
        fflush(stdout);

        total_hits += hits[0] + hits[1];

    }

    return total_hits / EXPERIMENT_ITERATIONS / iterations;
}

uint64_t hits_per_config[sizeof(test_n_histories) / sizeof(int)][sizeof(test_history_sizes) / sizeof(int)][sizeof(test_his_taken_branches) / sizeof(int)];

void do_all_history_configs(struct config * cfg, bool init_global_hits) {
    uint64_t avg_hits;

    for (size_t n_his_idx = 0; n_his_idx < sizeof(test_n_histories) / sizeof(int); n_his_idx++)
    {
        cfg->n_histories = test_n_histories[n_his_idx];

        for (size_t his_size_idx = 0; his_size_idx < sizeof(test_history_sizes) / sizeof(int); his_size_idx++)
        {
            cfg->history_size = test_history_sizes[his_size_idx];

            for (size_t his_taken_idx = 0; his_taken_idx < sizeof(test_his_taken_branches) / sizeof(int); his_taken_idx++)
            {
                cfg->his_taken_branches = test_his_taken_branches[his_taken_idx];
                if (cfg->his_taken_branches >= cfg->history_size) {
                    continue;
                }

                avg_hits = do_experiment(cfg);
                if (init_global_hits) {
                    hits_per_config[n_his_idx][his_size_idx][his_taken_idx] = avg_hits;
                }
            }
        }
    }

}

// Optimization: only do top 4 for next runs

typedef struct {
    uint64_t hits;
    int n_histories;
    int history_size;
    int his_taken;
} result_t;

void do_top_configs(struct config * cfg, result_t top[10]) {

    for (int i = 0; i < 10; i++) {
        cfg->n_histories = top[i].n_histories;
        cfg->history_size = top[i].history_size;
        cfg->his_taken_branches = top[i].his_taken;
        do_experiment(cfg);
    }
}


void get_top4_for_n_history(result_t top[10]) {
    for (int i = 0; i < 10; i++) {
        top[i].hits = 0;
    }

    size_t n_his = sizeof(test_n_histories) / sizeof(int);
    size_t n_history_sizes = sizeof(test_history_sizes) / sizeof(int);
    size_t n_taken = sizeof(test_his_taken_branches) / sizeof(int);

    for (size_t n = 0; n < n_his; n++) {
        for (size_t h = 0; h < n_history_sizes; h++) {
            for (size_t t = 0; t < n_taken; t++) {

                uint64_t val = (uint64_t) hits_per_config[n][h][t];

                for (int k = 0; k < 10; k++) {
                    if (val > top[k].hits) {

                        // Shift down
                        for (int s = 9; s > k; s--) {
                            top[s] = top[s - 1];
                        }

                        top[k].hits = val;
                        top[k].n_histories = test_n_histories[n];
                        top[k].history_size = test_history_sizes[h];
                        top[k].his_taken = test_his_taken_branches[t];
                        break;
                    }
                }
            }
        }
    }
}

#define ITERATIONS_INIT 100000


void print_cache_timing(uint8_t * fr_buf){

    printf("           | min |  q1 | med |  q3 | max\n");

    uint64_t times_c[ITERATIONS_INIT];
    uint64_t times_f[ITERATIONS_INIT];

    uint64_t hits_cached = 0;
    uint64_t hits_flushed = 0;
    // init
    for (size_t i = 0; i < ITERATIONS_INIT; i++)
    {
        maccess(fr_buf);
        fence();
        times_c[i] = load_time(fr_buf);
        if(times_c[i] < THR) {
            hits_cached++;
        }

        fence();
        flush(fr_buf);
        fence();
        times_f[i] = load_time(fr_buf);
        if(times_f[i] < THR) {
            hits_flushed++;
        }
    }

    sort_ascending(times_c, ITERATIONS_INIT);
    printf("    cached | %3lu | %3lu | %3lu | %3lu | %3lu\n",
		times_c[0], times_c[ITERATIONS_INIT/4], times_c[ITERATIONS_INIT/2], times_c[ITERATIONS_INIT*3/4], times_c[ITERATIONS_INIT-1]);


    sort_ascending(times_f, ITERATIONS_INIT);
    printf("   flushed | %3lu | %3lu | %3lu | %3lu | %3lu\n",
		times_f[0], times_f[ITERATIONS_INIT/4], times_f[ITERATIONS_INIT/2], times_f[ITERATIONS_INIT*3/4], times_f[ITERATIONS_INIT-1]);


    printf("Current threshold: %d\n", THR);
    printf("%10s %4s|%4s\n", "", "hit", "mis");
    printf("%10s %4lu|%4lu\n", "cached", hits_cached / 100, ITERATIONS_INIT / 100 - hits_cached / 100);
    printf("%10s %4lu|%4lu\n", "flushed", hits_flushed / 100, ITERATIONS_INIT / 100 - hits_flushed / 100);

}

void init_buffers(struct config * cfg) {
    for (int i = 0; i < N_FR_BUF; i++) {
        cfg->fr_buf[i] = mmap(NULL, 0x2000, PROT_READ|PROT_WRITE,
            MAP_PRIVATE|MAP_ANONYMOUS|MAP_POPULATE, -1, 0);
        assert(cfg->fr_buf[i] != (void *) -1);
        memset(cfg->fr_buf[i], 0x94, 0x2000);
    }

    for (int i = 0; i < MAX_N_HISTORIES; i++) {
        cfg->history[i] = calloc(sizeof(uint8_t), MAX_HISTORY_SIZE);
        assert(cfg->history[i] != (void *) -1);
    }

    ftable[1] = mmap(NULL, 0x1000, PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS|MAP_POPULATE, -1, 0);
    ftable[2] = mmap(NULL, 0x1000, PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS|MAP_POPULATE, -1, 0);
    restore_target();
}

int main(int argc, char **argv)
{
    struct config * cfg = calloc(sizeof(config), 1);
    pthread_t tid;

    init_buffers(cfg);

    pin_to_core(MAIN_CORE);
    assert(syscall(SYS_getcpu, &cfg->cpu_nr, NULL, NULL) == 0);
    printf("Running on CPU: %d\n", cfg->cpu_nr);
#if 1
    printf("============================ ENVIRONMENT INFO ============================\n");
    fflush(stdout);

    (system("grep '^model name' /proc/cpuinfo | head -n 1") + 1);
    (system("grep 'stepping\\|model\\|microcode\\|family' /proc/cpuinfo | grep -v 'model name' | head -n 4") + 1);
    (system("echo -n 'Linux version   : ' && uname -r") + 1);
    (system("echo 'Linux spectre_v2 mitigation info:' && echo -n '- ' && cat /sys/devices/system/cpu/vulnerabilities/spectre_v2 | cut -d' ' -f2-") + 1);

    printf("==========================================================================\n");
    fflush(stdout);
#endif

    print_cache_timing(cfg->fr_buf[0]);
    printf("==========================================================================\n");

#if 0
    size_t cycles =  get_busy_loop_cycles(0);
    printf("Usleep: %7d Passed cycles: %8ldk\n", 0, cycles / 1000);

    for (int idx = 0; idx < sizeof(test_usleep) / sizeof(size_t); idx++) {
        cycles =  get_busy_loop_cycles(test_usleep[idx]);
        printf("Usleep: %7ld Passed cycles: %8ldk\n", test_usleep[idx], cycles / 1000);
    }

    for (int idx = 0; idx < sizeof(test_n_forks) / sizeof(size_t); idx++) {
        fflush(stdout);
        cycles =  get_cycles_for_forks(test_n_forks[idx]);
        printf("Forks : %7ld Passed cycles: %8ldk\n", test_n_forks[idx], cycles / 1000);

    }
#endif

    // ---------------------------------------------------------------------------------
    // Test RSB stale entry use

    do_rsb_experiment(cfg);

    // ---------------------------------------------------------------------------------
    // base line
    cfg->usleep = 0;
    cfg->busy_cycles = 0;
    cfg->do_overwrite = 0;
    cfg->loop_type = LOOP_NONE;
    result_t top10[10];

    for (int train_type = 0; train_type < 3; train_type++)
    {
        cfg->train_type = train_type;

        do_all_history_configs(cfg, 1);

        get_top4_for_n_history(top10);

        printf("Top 10 for n_histories. Train type=%d\n", cfg->train_type);

        for (int i = 0; i < 10; i++) {
            printf("  hits=%lu, n_histories=%d, history_size=%d, taken=%d\n",
                top10[i].hits, top10[i].n_histories, top10[i].history_size, top10[i].his_taken);
        }
    }

    // ---------------------------------------------------------------------------------
    // BUSY loop

    cfg->loop_type = LOOP_BUSY;
    for (int idx = 0; idx < sizeof(test_busy_loop) / sizeof(size_t); idx++) {
        cfg->busy_cycles = test_busy_loop[idx];
        #ifdef RUN_ALL_CONFIGS
            do_all_history_configs(cfg, 0);
        #else
        do_top_configs(cfg, top10);
        #endif
    }

    // ---------------------------------------------------------------------------------
    // BUSY loop FORK

    cfg->loop_type = LOOP_BUSY_FORK;
    for (int idx = 0; idx < sizeof(test_busy_loop) / sizeof(size_t); idx++) {
        cfg->n_forks = test_n_forks[idx];
        #ifdef RUN_ALL_CONFIGS
        do_all_history_configs(cfg, 0);
        #else
        do_top_configs(cfg, top10);
        #endif
    }

    // ---------------------------------------------------------------------------------
    // BUSY loop with overwrite

    cfg->loop_type = LOOP_BUSY;
    cfg->busy_cycles = 0;
    cfg->do_overwrite = 1;
    do_top_configs(cfg, top10);

    for (int idx = 0; idx < sizeof(test_busy_loop) / sizeof(size_t); idx++) {
        cfg->busy_cycles = test_busy_loop[idx];
        #ifdef RUN_ALL_CONFIGS
        do_all_history_configs(cfg, 0);
        #else
        do_top_configs(cfg, top10);
        #endif
    }



    free(cfg);

}
