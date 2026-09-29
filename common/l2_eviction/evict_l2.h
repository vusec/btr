#ifndef _EVICT_SYS_TABLE_L2_H_
#define _EVICT_SYS_TABLE_L2_H_

#if defined(INTEL_10_GEN)
    #define L1_WAYS (8)
    #define L1_SETS (64)
    #define L2_WAYS (4)
    #define L2_SETS (1024)
    #define L3_WAYS (16)
    #define L3_SETS (16384)
    #define L1_EVICT_SIZE (8 + 6)
    #define L2_EVICT_SIZE (4 + 6)
    #define L3_EVICT_SIZE (32) // (12 + 8)

#elif defined(INTEL_11_GEN)
    #define L1_WAYS 12
    #define L1_SETS (64)
    #define L2_WAYS 8
    #define L2_SETS 1024
    #define L2_EVICT_SIZE (8 + 6)

#elif defined(GOLDEN_COVE) || defined(INTEL_13_GEN) || defined(INTEL_14_GEN) || defined(RAPTOR_COVE) || \
        defined(INTEL_ULTRA_1) || defined(REDWOOD_COVE)
    #define L1_WAYS (12)
    #define L1_SETS (64)
    #define L2_WAYS (16)
    #define L2_SETS (2048)
    #define L3_WAYS (12)
    #define L3_SETS (49152)
    #define L1_EVICT_SIZE (12 + 8)
    #define L2_EVICT_SIZE (L2_WAYS + 8)
    #define L3_EVICT_SIZE (2000)


#elif defined(LION_COVE)
    #define L1_WAYS (12)
    #define L1_SETS (64)
    #define L2_WAYS (12)
    #define L2_SETS (4096)
    #define L3_WAYS (12)
    #define L3_SETS (49152)

    #define L1_EVICT_SIZE (L1_WAYS + 8)
    #define L2_EVICT_SIZE (L2_WAYS + 32)
    #define L3_EVICT_SIZE (2000)

#else
    #error "Not supported micro-architecture"
    // silence undefined errors
    #define L1_WAYS 1
    #define L1_SETS 1
    #define L2_WAYS 1
    #define L2_SETS 1
    #define L3_WAYS 1
    #define L3_SETS 1
    #define L1_EVICT_SIZE 1
    #define L2_EVICT_SIZE 1
    #define L3_EVICT_SIZE 1

#endif

#include <assert.h>


void build_ev_set_l2(uint64_t target, void ** ev_set);
void build_ev_set_l1(uint64_t target, void ** ev_set);


__always_inline void evict(void **ev_set)
{
	void **start = (void **)ev_set[0];
	void **p = start;
	do {
		p = *p;
        asm volatile("lfence\n");
        // asm volatile ("xor %%rax, %%rax\ncpuid\n\t" ::: "%rax", "%rbx", "%rcx", "%rdx");
	} while (p != start);
}

#endif //_EVICT_SYS_TABLE_L2_H_
