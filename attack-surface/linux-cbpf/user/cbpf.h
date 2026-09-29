/*
 * Branch Target Reuse (BTR)
 * April 30th 2026
 */

#ifndef _CBPF_H_
#define _CBPF_H_

void fork_insert_program_2MB();
void fork_insert_program_4K();
void fork_insert_program_2K();
void fork_insert_allow_all_prog(int n);

void fork_reserve_n_bytes(uint64_t bytes);

void insert_training_branch(uint64_t branch_offset, uint64_t target_offset);

void insert_page_size_prog();
void insert_double_page_size_prog();
void insert_allow_all_prog();


void initialize_cbpf(uint8_t constant_blind_safe);


#endif // _CBPF_H_
