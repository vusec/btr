#ifndef _EVICT_H_
#define _EVICT_H_

#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include "common.h"

//Max possible ev set size
#define EV_SET_MAX (100000 * 2)

typedef struct ev_set_t {
    uint32_t size;
    uint32_t front;
    uint32_t rear;
    uint8_t *ptr[EV_SET_MAX];
} ev_set_t;


void enqueue(ev_set_t *ev, uint8_t *ptr) {
    assert(ev->size < EV_SET_MAX);

    ev->ptr[ev->rear] = ptr;
    ev->size++;
    ev->rear = (ev->rear + 1) % EV_SET_MAX;
}

void enqueue_head(ev_set_t *ev, uint8_t *ptr) {
    assert(ev->size < EV_SET_MAX);

    ev->front = (ev->front - 1) % EV_SET_MAX;
    ev->ptr[ev->front] = ptr;
    ev->size++;
}

uint8_t* dequeue(ev_set_t *ev) {
    uint8_t *ret;
    assert(ev->size > 0);

    ret = ev->ptr[ev->front];
    ev->front = (ev->front + 1) % EV_SET_MAX;
    ev->size--;

    return ret;
}

void print_ev(ev_set_t *ev) {
    printf("ev_set size = %d\n", ev->size);
    printf("front %d\n", ev->front);
    printf("rear %d\n", ev->rear);

    int idx = ev->front;
    printf("{");
    for(int i=0; i<ev->size; i++) {
        printf("%p, ", ev->ptr[idx]);
        idx = (idx + 1) % EV_SET_MAX;
    }
    printf("}\n");
}

static __always_inline void evict_ev_set(ev_set_t *ev) {
    int idx;

    for(int j=0; j<1; j++)
    {
        idx = ev->front;
        for(int i=0; i<ev->size; i++) {
            maccess(ev->ptr[idx]);
            idx = (idx + 1) % EV_SET_MAX;
        }
    }
}

#endif
