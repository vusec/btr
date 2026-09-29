/*
 * Branch Target Reuse (BTR)
 * April 30th 2026
 */

#ifndef _CONFIG_H_
#define _CONFIG_H_

#include <stdint.h>

#define MAX_HISTORY_SIZE 512
#define MAX_N_HISTORIES 9000

#define N_FR_BUF 2


#define X86                 (1)
#define AARCH64             (2)

#ifdef i9_14900K
    #define ARCH_S "i9-14900K"
    #define MAIN_CORE 2
    #define THR 80
    #define ARCH X86
#elif defined(U9_285K)
    #define ARCH_S "9-285K"
    #define MAIN_CORE 2
    #define THR 80
    #define ARCH X86
#elif defined(AMD_7950X)
    #define ARCH_S "7950X"
    #define MAIN_CORE 2
    #define THR 100
    #define ARCH X86
#elif defined(CORTEX_X3)
    #define ARCH_S "Cortex-X3"
    #define MAIN_CORE 8
    #define THR 90
    #define ARCH AARCH64
    // Cortex CX3 applies heavy prefetching
    // To prevent noise (second buffer being prefetched), we perform
    // the reloads with a separate training run
    #define FR_BUF_SEQUENTIAL_TESTING
#elif defined(Cortex_A76)
    #define ARCH_S "Cortex-A76"
    #define MAIN_CORE 1
    #define THR 130
    #define ARCH AARCH64
#else
    #error "Not supported micro-architecture"
    #define ARCH_S ""
    #define MAIN_CORE -1
    #define THR -1

#endif


struct config {
    int cpu_nr;
    uint8_t * fr_buf[N_FR_BUF];
    uint8_t * history[MAX_N_HISTORIES];

    int n_histories;
    int history_size;
    int his_taken_branches;

    size_t busy_cycles;
    size_t usleep;
    int n_forks;
    int loop_type;
    int do_overwrite;
    int train_type;
} typedef config;

#endif //_CONFIG_H_

