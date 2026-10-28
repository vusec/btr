/*
 * Branch Target Reuse (BTR)
 * April 30th 2026
 */

#ifndef _HELPER_H_
#define _HELPER_H_

#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <malloc.h>
#include <string.h>

#include "config.h"
#include "common.h"

static __always_inline void get_shuffled_array(int min, int max, int array[]) {

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


void randomize_history(struct config * cfg, uint8_t * cond_array);


static __always_inline uint64_t get_45bit_random_value() {
    return (((uint64_t) rand() & 0x7fffLU) << 30) |
            (((uint64_t) rand() & 0x7fffLU) << 15) |
            ((uint64_t) rand() & 0x7fffLU);

}




#endif //_HELPER_H_
