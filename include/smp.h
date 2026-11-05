#ifndef OMEGA_SMP_H
#define OMEGA_SMP_H
#include "ipc.h"
#include "proof_sched.h"
#define SMP_MESSAGES 65536ULL
void smp_start(omega_ring_t *ring,omega_task_t *consumer);
void smp_wait(void);
void smp_timer_start(void);
void smp_timer_stop(void);
void smp_timer_eoi(void);
#endif
