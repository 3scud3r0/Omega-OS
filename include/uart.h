#ifndef OMEGA_UART_H
#define OMEGA_UART_H
#include "types.h"
void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_put_hex(uint64_t v);
void uart_put_dec(uint64_t v);
#endif
