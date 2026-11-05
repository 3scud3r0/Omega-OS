#ifndef OMEGA_TYPES_H
#define OMEGA_TYPES_H
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef signed long long int64_t;
typedef __SIZE_TYPE__ size_t;
typedef _Bool bool;
#define true 1
#define false 0
#define ALIGN_64 __attribute__((aligned(64)))
#define ALIGN_4K __attribute__((aligned(4096)))
static inline void outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t inb(uint16_t p) { uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void compiler_barrier(void) { __asm__ volatile("":::"memory"); }
static inline void outw(uint16_t p,uint16_t v) { __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline void outl(uint16_t p,uint32_t v) { __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline uint16_t inw(uint16_t p) { uint16_t v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint32_t inl(uint16_t p) { uint32_t v; __asm__ volatile("inl %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void cpu_pause(void) { __asm__ volatile("pause":::"memory"); }
static inline void mfence(void) { __asm__ volatile("mfence":::"memory"); }
static inline void lfence(void) { __asm__ volatile("lfence":::"memory"); }
static inline void sfence(void) { __asm__ volatile("sfence":::"memory"); }
static inline void invlpg(uint64_t v) { __asm__ volatile("invlpg (%0)"::"r"(v):"memory"); }
static inline uint64_t rdtsc(void) { uint32_t l,h; lfence(); __asm__ volatile("rdtsc":"=a"(l),"=d"(h)); return ((uint64_t)h<<32)|l; }
void *memset(void *p, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void panic(const char *reason) __attribute__((noreturn));
#endif
