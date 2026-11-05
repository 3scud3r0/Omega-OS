#ifndef OMEGA_MMU_H
#define OMEGA_MMU_H
#include "types.h"
#define PAGE_SIZE 4096ULL
#define PAGE_PRESENT 1ULL
#define PAGE_WRITE 2ULL
#define PAGE_USER 4ULL
#define PAGE_PWT 8ULL
#define PAGE_PCD 16ULL
#define PAGE_DIRTY 64ULL
#define PHYS_LIMIT (64ULL*1024*1024)
extern uint64_t boot_pml4[512];
void pmm_init(uint64_t available_end,uint64_t reserved_end);
uint64_t pmm_alloc_frame(void);
bool pmm_free_frame(uint64_t p);
bool mmu_map(uint64_t *root,uint64_t va,uint64_t pa,uint64_t flags);
bool mmu_map_device(uint64_t pa,uint64_t length);
void sls_test_crash_after(unsigned boundary);
bool sls_register(uint64_t va);
bool sls_contains(uint64_t va);
bool sls_persist(uint64_t va,const void *data);
bool sls_restore(uint64_t va);
uint64_t mmu_sls_commit(uint64_t *root);
#endif
