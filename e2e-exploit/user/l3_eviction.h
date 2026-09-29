/*
 * Branch Target Reuse (BTR)
 * April 30th 2026
 */

#ifndef _L3_EV_EVICTION_H_
#define _L3_EV_EVICTION_H_

#include <stdint.h>

void find_l3_ev_set(struct config * cfg, uint64_t target, void ** ev_set_l2, void ** ev_set_l3);

#endif //_L3_EVICTION_H_
