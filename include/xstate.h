#ifndef OMEGA_XSTATE_H
#define OMEGA_XSTATE_H
#include "types.h"
/* Standard-format x87/SSE/YMM XSAVE. No AVX-512 state is enabled. */
typedef struct {
    uint8_t bytes[832];
} __attribute__((aligned(64))) xstate_t;
void xstate_init_cpu(void);
void xstate_reset(xstate_t *state);
void xstate_save(xstate_t *state);
void xstate_restore(const xstate_t *state);
uint64_t xstate_features(void);
#endif
