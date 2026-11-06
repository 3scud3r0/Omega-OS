#ifndef OMEGA_JIT_H
#define OMEGA_JIT_H
#include "bytecode.h"
#define JIT_VA 0x83000000ULL
#define JIT_GAS 2048U
/* Certificate is a finite, typed expression. It is never the executable.
 * Auditing re-encodes every instruction, immediate and branch target. */
bool jit_compile(const void *certificate,size_t n,uint8_t code[4096],size_t *length);
bool jit_audit(const void *certificate,size_t n,const uint8_t *code,size_t length);
#endif
