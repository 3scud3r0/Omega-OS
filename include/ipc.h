#ifndef OMEGA_IPC_H
#define OMEGA_IPC_H
#include "types.h"
#define RING_CAPACITY 1024U
typedef struct { uint64_t head ALIGN_64; uint64_t tail ALIGN_64; uint64_t slots[RING_CAPACITY] ALIGN_64; } omega_ring_t;
void ring_init(omega_ring_t *r);
bool ring_push(omega_ring_t *r, uint64_t v);
bool ring_pop(omega_ring_t *r, uint64_t *v);
#endif
