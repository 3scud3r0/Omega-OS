#ifndef OMEGA_MMU_H
#define OMEGA_MMU_H
#include "types.h"
#define PAGE_SIZE 4096ULL
#define PAGE_PRESENT 1ULL
#define PAGE_WRITE 2ULL
#define PAGE_DIRTY 64ULL
#define PHYS_LIMIT (64ULL*1024*1024)
extern uint64_t boot_pml4[512];
void pmm_init(uint64_t available_end,uint64_t reserved_end);
uint64_t pmm_alloc_frame(void);
bool pmm_free_frame(uint64_t p);
bool mmu_map(uint64_t *root,uint64_t va,uint64_t pa,uint64_t flags);
uint64_t mmu_sls_commit(uint64_t *root);
#endif
