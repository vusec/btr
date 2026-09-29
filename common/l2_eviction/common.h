#ifndef _COMMON_H_
#define _COMMON_H_

#include <unistd.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <malloc.h>

#define HUGE_PAGE_SIZE (1 << 21)
#define PAGE_SIZE (1 << 12)

static int get_rss_of_addr(void * addr)
{
    void * start = 0;
    int rss = -1;
    char line[256];

    FILE* file = fopen("/proc/self/smaps","r");

    if (!file) {
        printf("Error opening /proc/self/smaps file!\n");
        exit(EXIT_FAILURE);
    }

    while(fgets(line, sizeof(line), file)) {
        // Find the address
        if(sscanf(line, "%p-%*[^\n]\n", &start) != 1 && start == addr)
        {
            // Find the RSS field
            while(fgets(line, sizeof(line), file)) {

                if(sscanf(line, "Rss: %d kB\n", &rss) == 1) {
                    break;
                }
            }

            break;

        }

    }

    fclose(file);

    return rss;
}

static uint8_t * allocate_huge_page()
{
    uint8_t * addr = memalign(HUGE_PAGE_SIZE, HUGE_PAGE_SIZE);

    madvise(addr, HUGE_PAGE_SIZE, MADV_HUGEPAGE);
    *(volatile uint8_t *) addr = 1;

    assert(get_rss_of_addr(addr) == 2048);

    return addr;
}


// /* Allocate a hugepage worth of physical contiguous memory and (void *)mmap it into
//  * the virtual address space (optionally at @addr). If @split, then split it up
//  * into 4kb pages; the last page is read-only, the rest read+write.
//  */
// void *alloc_contiguous_pages(void *addr, int split)
// {
// 	char *p;
// 	uint64_t base;

// 	if (!addr) {
// 		// Find a suitable hugepage aligned address for our eviction buffer.
// 		p = (char *)mmap(NULL, 2*HUGE_PAGE_SIZE, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
// 		if (p == (void *)-1) {
// 			pr_err("mmap: %s\n", strerror(errno));
// 			fail("alloc_contiguous_pages alignment mmap failed");
// 		}
// 		if (munmap(p, 2*HUGE_PAGE_SIZE) < 0) {
// 			pr_err("munmap: %s\n", strerror(errno));
// 			fail("alloc_contiguous_pages munmap failed");
// 		}
// 		base = (uint64_t)p;
// 		while (base % HUGE_PAGE_SIZE)
// 			base += HUGE_PAGE_SIZE;
// 		addr = (void *)base;
// 	}

// 	assert((uint64_t)addr % HUGE_PAGE_SIZE == 0);

// 	// mmap the virtual memory at the chosen address.
// 	p = (char *)mmap(addr, HUGE_PAGE_SIZE, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
// 	if (p == (void *)-1) {
// 		pr_err("mmap: %s\n", strerror(errno));
// 		fail("alloc_contiguous_pages buf mmap failed");
// 	}
// 	if (p != addr)
// 		fail("alloc_contiguous_pages cant mmap that exact address");

// 	// Turn it into a hugepage.
// 	if (madvise(p, HUGE_PAGE_SIZE, MADV_HUGEPAGE) < 0) {
// 		pr_err("madvise: %s\n", strerror(errno));
// 		fail("alloc_contiguous_pages madvise failed");
// 	}

// 	// Populate the hugepage, and check it is indeed huge.
// 	assert(rss(p) == 0);
// 	*p = '\0';
// 	assert(rss(p) == HUGE_PAGE_SIZE); // hugeness check

// 	if (split) {
// 		// Split the huge page table into 512 small page tables.
// 		if (mprotect(p + HUGE_PAGE_SIZE - PAGE_SIZE, PAGE_SIZE, PROT_READ) < 0) {
// 			pr_err("mprotect: %s\n", strerror(errno));
// 			fail("alloc_contiguous_pages mprotect failed");
// 		}
// 	}

// 	return p;
// }



#endif //_COMMON_H_
