#ifndef OMEGA_PROOF_SCHED_H
#define OMEGA_PROOF_SCHED_H
#include "rational.h"
#define STATE_DIM 4
typedef struct { uint64_t id; omega_rational_t state[STATE_DIM]; uint64_t proof_hash; uint64_t epoch; bool quarantined; } omega_task_t;
uint64_t omega_contract_token(const omega_task_t *task);
bool omega_verify_contract(const omega_task_t *task);
bool omega_checkpoint(omega_task_t *task,omega_task_t *checkpoint);
bool omega_admit(omega_task_t *task,const omega_task_t *checkpoint);
#endif
