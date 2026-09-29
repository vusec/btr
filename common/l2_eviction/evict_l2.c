#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <malloc.h>
#include <math.h>


#include "common.h"
#include "evict_l2.h"

#define PAGE_SIZE (1 << 12)
#define HUGE_PAGE_SIZE (1 << 21)

void link_ev_set(void ** ev_set, int ev_set_size) {

    void **next;
	for (int i = 0; i < ev_set_size; i++) {
		next = ev_set[i];
		*next = ev_set[(i + 1) % ev_set_size];
	}

}

void build_ev_set_l2(uint64_t target, void ** ev_set) {
    uint8_t * page_2mb[8];

    int max_per_page = HUGE_PAGE_SIZE / L2_SETS / 64;
    int n_pages = ceil(L2_EVICT_SIZE / (double) max_per_page);

    for (int i = 0; i < n_pages; i++)
    {
        page_2mb[i] = allocate_huge_page();
    }

    uint64_t offset = target & ((64 * L2_SETS) - 1);
    printf("[+] l2 offset: %lx\n", offset);

    int page_idx = -1;

    for (int i = 0; i < L2_EVICT_SIZE; i++)
    {
        if (i % max_per_page == 0) {
            page_idx += 1;
        }

        uint8_t * addr = page_2mb[page_idx] + offset + (i%max_per_page * L2_SETS * 64);
        assert(addr < (page_2mb[page_idx] + (1 << 21)));
        ev_set[i] = addr;
    }

    link_ev_set(ev_set, L2_EVICT_SIZE);

}


void build_ev_set_l1(uint64_t target, void ** ev_set) {
    uint8_t * page_2mb[8];

    int max_per_page = HUGE_PAGE_SIZE / L1_SETS / 64;
    int n_pages = ceil(L1_EVICT_SIZE / (double) max_per_page);

    for (int i = 0; i < n_pages; i++)
    {
        page_2mb[i] = allocate_huge_page();
    }

    // printf("h_page : %p\n", page_2mb[0]);


    uint64_t offset = target & (PAGE_SIZE - 1);

    int page_idx = -1;

    for (int i = 0; i < L1_EVICT_SIZE; i++)
    {
        if (i % max_per_page == 0) {
            page_idx += 1;
        }

        uint8_t * addr = page_2mb[page_idx] + offset + (i%max_per_page * L1_SETS * 64);
        // printf("l1 Addr: %p\n", addr);
        assert(addr < (page_2mb[page_idx] + (1 << 21)));
        ev_set[i] = addr;
    }

    link_ev_set(ev_set, L1_EVICT_SIZE);

}

