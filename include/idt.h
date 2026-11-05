#ifndef OMEGA_IDT_H
#define OMEGA_IDT_H
#include "types.h"
#define USER_CS 0x23U
#define USER_DS 0x1bU
typedef struct __attribute__((packed)) {
 uint16_t offset_lo,selector; uint8_t ist,attributes; uint16_t offset_mid;
 uint32_t offset_hi,reserved;
} idt_entry_t;
typedef struct {
 uint64_t r15,r14,r13,r12,r11,r10,r9,r8,rdi,rsi,rbp,rdx,rcx,rbx,rax;
 uint64_t vector,error,rip,cs,rflags,rsp,ss;
} interrupt_frame_t;
void idt_init(void);
void idt_init_cpu(unsigned cpu);
void isolation_test(uint64_t task_va);
uint64_t exception_dispatch(interrupt_frame_t *frame) __attribute__((used,externally_visible));
#endif
