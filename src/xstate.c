#include "xstate.h"
#include "uart.h"
static bool use_xsave, initialized;
static uint64_t mask;
static xstate_t initial;
static void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}
void xstate_init_cpu(void) {
    uint32_t a, b, c, d;
    uint64_t cr0, cr4, enabled = 3;
    cpuid(1, 0, &a, &b, &c, &d);
    bool xs = (c & (1U << 26)) != 0, avx = (c & (1U << 28)) != 0;
    __asm__ volatile("movq %%cr0,%0" : "=r"(cr0));
    cr0 = (cr0 & ~12ULL) | 2;
    __asm__ volatile("movq %0,%%cr0" ::"r"(cr0) : "memory");
    __asm__ volatile("movq %%cr4,%0" : "=r"(cr4));
    cr4 |= (1ULL << 9) | (1ULL << 10);
    if (xs)
        cr4 |= 1ULL << 18;
    __asm__ volatile("movq %0,%%cr4" ::"r"(cr4) : "memory");
    if (xs) {
        cpuid(13, 0, &a, &b, &c, &d);
        if (avx && (a & 7) == 7)
            enabled = 7;
        __asm__ volatile("xsetbv" ::"c"(0), "a"((uint32_t)enabled), "d"(0) : "memory");
        cpuid(13, 0, &a, &b, &c, &d);
        if (b > sizeof initial)
            panic("XSAVE enabled state exceeds context");
    }
    if (initialized && (use_xsave != xs || mask != (xs ? enabled : 0)))
        panic("heterogeneous XSAVE feature set");
    if (!initialized) {
        use_xsave = xs;
        mask = xs ? enabled : 0;
        memset(&initial, 0, sizeof initial);
        initial.bytes[0] = 0x7f;
        initial.bytes[1] = 3;
        initial.bytes[24] = 0x80;
        initial.bytes[25] = 0x1f;
        initialized = true;
    }
    xstate_restore(&initial);
}
void xstate_reset(xstate_t *s) { memcpy(s, &initial, sizeof *s); }
void xstate_save(xstate_t *s) {
    if (use_xsave)
        __asm__ volatile("xsave64 %0" : "=m"(*s) : "a"((uint32_t)mask), "d"(0) : "memory");
    else
        __asm__ volatile("fxsave64 %0" : "=m"(*s)::"memory");
}
void xstate_restore(const xstate_t *s) {
    if (use_xsave)
        __asm__ volatile("xrstor64 %0" ::"m"(*s), "a"((uint32_t)mask), "d"(0) : "memory");
    else
        __asm__ volatile("fxrstor64 %0" ::"m"(*s) : "memory");
}
uint64_t xstate_features(void) { return mask; }
