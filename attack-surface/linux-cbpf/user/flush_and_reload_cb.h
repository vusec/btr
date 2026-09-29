/*
 * Branch Target Reuse (BTR)
 * April 30th 2026
 */

#ifndef _FLUSH_AND_RELOAD_CB_H_
#define _FLUSH_AND_RELOAD_CB_H_

#include "flush_and_reload.h"
#include <stdint.h>

uint64_t cb_do_flush_and_reload(struct config * cfg, uint64_t iterations, uint8_t ret_on_hit);
int cb_do_train_and_reload(struct config * cfg, uint64_t iterations, uint8_t ret_on_hit);

void cb_print_leakage_rate(struct config * cfg, uint64_t iterations);

void cb_set_load_chain_leak_secret(struct config * cfg);
void cb_set_load_chain_n_loads(struct config * cfg, int number_of_loads);

void cb_leak_test_leakage_rate(struct config * cfg);
void cb_leak_dummy_secret(struct config * cfg);

uint64_t cb_leak_64bit_value_forwards(struct config *, uint8_t * address, uint32_t prefix);



#endif //_FLUSH_AND_RELOAD_H_
