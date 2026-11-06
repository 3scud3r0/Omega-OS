#ifndef OMEGA_INCUBATOR_H
#define OMEGA_INCUBATOR_H
#include "jit.h"
#define INCUBATOR_SLOTS 16U
typedef struct {uint64_t lineage,generation,cycles,bytes;omega_rational_t value;char genome[96];} omega_organism_t;
void incubator_run(void);
bool incubator_call(void *function,omega_rational_t *out);
#endif
