#include "mmu.h"
#include "uart.h"
#define MASK 0x000ffffffffff000ULL
static uint64_t free_bits[256] ALIGN_64;
static uint64_t summary[4] ALIGN_64;
static uint64_t top;
static uint64_t first_frame,last_frame;
void pmm_init(uint64_t end,uint64_t reserved) {
 uint64_t f;
 memset(free_bits,0,sizeof free_bits); memset(summary,0,sizeof summary); top=0;
 first_frame=(reserved+4095)/4096; last_frame=(end<PHYS_LIMIT?end:PHYS_LIMIT)/4096;
 for(f=first_frame;f<last_frame;f++) { uint64_t w=f/64,s=w/64; free_bits[w]|=1ULL<<(f%64); summary[s]|=1ULL<<(w%64); top|=1ULL<<s; }
}
uint64_t pmm_alloc_frame(void) {
 uint64_t s,w,f;
 if(!top) return 0;
 s=(uint64_t)__builtin_ctzll(top); w=s*64+(uint64_t)__builtin_ctzll(summary[s]);
 f=w*64+(uint64_t)__builtin_ctzll(free_bits[w]); free_bits[w]&=~(1ULL<<(f%64));
 if(!free_bits[w]) { summary[s]&=~(1ULL<<(w%64)); if(!summary[s]) top&=~(1ULL<<s); }
 memset((void *)(f*4096),0,4096); return f*4096;
}
bool pmm_free_frame(uint64_t p) {
 uint64_t f=p/4096,w=f/64,s=w/64;
 if(p%4096||f<first_frame||f>=last_frame||(free_bits[w]&(1ULL<<(f%64)))) return false;
 free_bits[w]|=1ULL<<(f%64); summary[s]|=1ULL<<(w%64); top|=1ULL<<s; return true;
}
bool mmu_map(uint64_t *root,uint64_t va,uint64_t pa,uint64_t flags) {
 uint64_t *t=root; unsigned shift;
 if((va|pa)&4095 || pa>=PHYS_LIMIT || (va>>47)!=0 || (flags&~(PAGE_PRESENT|PAGE_WRITE))) return false;
 for(shift=39;shift>12;shift-=9) { uint64_t *e=&t[(va>>shift)&511];
  if(!(*e&PAGE_PRESENT)) { uint64_t p=pmm_alloc_frame(); if(!p) return false; *e=p|PAGE_PRESENT|PAGE_WRITE; }
  if(*e&128) return false;
  t=(uint64_t *)(*e&MASK);
 }
 t[(va>>12)&511]=pa|flags|PAGE_PRESENT; invlpg(va); return true;
}
static uint64_t scan(uint64_t *t,unsigned level,uint64_t base) {
 unsigned i; uint64_t count=0;
 for(i=0;i<512;i++) { uint64_t e=__atomic_load_n(&t[i],__ATOMIC_RELAXED),va=base|((uint64_t)i<<(12+9*(level-1)));
  if(!(e&PAGE_PRESENT)) continue;
  if(level>1) { if(e&128) panic("unexpected huge page"); count+=scan((uint64_t *)(e&MASK),level-1,va); }
  else if(e&PAGE_DIRTY) { uart_puts("SLS dirty "); uart_put_hex(va); uart_puts(" checkpoint tracking (RAM)\n");
   __atomic_fetch_and(&t[i],~PAGE_DIRTY,__ATOMIC_ACQ_REL); invlpg(va); count++;
  }
 }
 return count;
}
uint64_t mmu_sls_commit(uint64_t *root) { mfence(); return scan(root,4,0); }
