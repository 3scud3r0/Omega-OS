#ifndef OMEGA_RATIONAL_H
#define OMEGA_RATIONAL_H
#include "types.h"
typedef struct { int64_t num; int64_t den; } omega_rational_t;
bool rational_make(int64_t n,int64_t d,omega_rational_t *r);
bool rational_add(omega_rational_t a,omega_rational_t b,omega_rational_t *r);
bool rational_sub(omega_rational_t a,omega_rational_t b,omega_rational_t *r);
bool rational_mul(omega_rational_t a,omega_rational_t b,omega_rational_t *r);
bool rational_cmp(omega_rational_t a,omega_rational_t b,int *result);
bool rational_valid(omega_rational_t r);
#endif
