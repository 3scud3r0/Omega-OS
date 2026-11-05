#ifndef OMEGA_ADMISSION_H
#define OMEGA_ADMISSION_H
#include "types.h"
bool vm_verify(const uint8_t *code,size_t length);
const uint8_t *admission_code(size_t *length);
const uint8_t *admission_compiler(size_t *length);
#endif
