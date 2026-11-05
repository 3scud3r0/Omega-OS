#ifndef OMEGA_BYTECODE_H
#define OMEGA_BYTECODE_H
#include "rational.h"
#define BYTECODE_MAGIC 0x3343424fULL
bool bytecode_run(const void *capsule,size_t size,omega_rational_t *out);
#endif
