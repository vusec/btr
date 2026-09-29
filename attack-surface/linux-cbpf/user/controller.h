#ifndef _CONTROLLER_H_
#define _CONTROLLER_H_

#include <stdint.h>
#include <stdatomic.h>


enum controller_status_t {
    CONT_READY,
    CONT_TRAINING,
    CONT_GADGET_INSERTED,
    CONT_CLEAN_UP
};

extern atomic_int controller_status;

void * thread_controller(void * arg);

#endif // _CONTROLLER_H_
