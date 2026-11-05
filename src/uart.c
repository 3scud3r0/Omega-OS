#include "uart.h"
void uart_init(void) {
 outb(0x3f9,0); outb(0x3fb,0x80); outb(0x3f8,1); outb(0x3f9,0);
 outb(0x3fb,3); outb(0x3fa,0xc7); outb(0x3fc,0x0b);
}
void uart_putc(char c) { while (!(inb(0x3fd)&0x20)) { __asm__ volatile("pause"); } outb(0x3f8,(uint8_t)c); }
void uart_puts(const char *s) { while (*s) uart_putc(*s++); }
void uart_put_hex(uint64_t v) { unsigned i; uart_puts("0x"); for(i=0;i<16;i++) uart_putc("0123456789abcdef"[(v>>(60-4*i))&15]); }
void uart_put_dec(uint64_t v) { char b[20]; unsigned n=0; do { b[n++]=(char)('0'+v%10); v/=10; } while(v); while(n) uart_putc(b[--n]); }
