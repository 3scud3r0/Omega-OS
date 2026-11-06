#ifndef OMEGA_RUNTIME_H
#define OMEGA_RUNTIME_H
#include "idt.h"
void runtime_resume(void);
const void *runtime_nomination(void);
bool runtime_run(void);
void runtime_source(const char *source,size_t n);
bool runtime_active(void);
uint64_t runtime_trap(interrupt_frame_t *frame);
uint64_t syscall_dispatch(interrupt_frame_t *frame) __attribute__((used,externally_visible));
#endif
