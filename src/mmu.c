#include "mmu.h"
#include "uart.h"
#define MASK 0x000ffffffffff000ULL
static uint64_t free_bits[256] ALIGN_64;
static uint64_t summary[4] ALIGN_64;
static uint64_t top;
static uint64_t first_frame,last_frame;
static bool hardened;
uint64_t *mmu_leaf(uint64_t *root,uint64_t va){
 uint64_t *t=root;for(unsigned shift=39;shift>12;shift-=9){uint64_t e=t[(va>>shift)&511];if(!(e&1)||(e&128))return (void *)0;t=(uint64_t *)(e&MASK);}return &t[(va>>12)&511];
}
bool mmu_identity(uint64_t p,bool writable){uint64_t *e=mmu_leaf(boot_pml4,p);if(!e||!(*e&1))return false;*e=(*e|PAGE_NX)&~PAGE_WRITE;if(writable)*e|=PAGE_WRITE;invlpg(p);return true;}
void mmu_harden(void){
 extern uint8_t __text_start[],__text_end[],__rodata_end[];
 for(uint64_t p=0;p<PHYS_LIMIT;p+=4096){uint64_t *e=mmu_leaf(boot_pml4,p);if(!e||!(*e&1))panic("identity protection");
  *e|=PAGE_NX;
  if(p>=(uint64_t)__text_start&&p<(uint64_t)__text_end)*e=(*e&~(PAGE_NX|PAGE_WRITE));
  else if(p>=(uint64_t)__text_end&&p<(uint64_t)__rodata_end)*e&=~PAGE_WRITE;
 }
 /* Retire writable/executable aliases used exclusively by the V1 fault probe. */
 for(uint64_t p=0x50000000;p<=0x50001000;p+=4096){uint64_t *e=mmu_leaf(boot_pml4,p);if(e)*e=0;}
 __asm__ volatile("movq %0,%%cr3"::"r"((uint64_t)boot_pml4):"memory");hardened=true;uart_puts("MMU steady-state W^X: kernel RX, data NX, executable aliases read-only\n");
}
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
 if(hardened&&!mmu_identity(p,true))return false;
 free_bits[w]|=1ULL<<(f%64); summary[s]|=1ULL<<(w%64); top|=1ULL<<s; return true;
}
bool mmu_map(uint64_t *root,uint64_t va,uint64_t pa,uint64_t flags) {
 uint64_t *t=root; unsigned shift;
 if((va|pa)&4095 || pa>=0x100000000ULL || (va>>47)!=0 || (flags&~(PAGE_PRESENT|PAGE_WRITE|PAGE_USER|PAGE_PWT|PAGE_PCD|PAGE_NX))) return false;
 for(shift=39;shift>12;shift-=9) { uint64_t *e=&t[(va>>shift)&511];
  if(!(*e&PAGE_PRESENT)) { uint64_t p=pmm_alloc_frame(); if(!p) return false; *e=p|PAGE_PRESENT|PAGE_WRITE; }
  if(*e&128) return false;
  if(flags&PAGE_USER) *e|=PAGE_USER;
  t=(uint64_t *)(*e&MASK);
 }
 if(hardened&&(flags&PAGE_WRITE))flags|=PAGE_NX;
 if(hardened&&(flags&PAGE_USER)&&!(flags&PAGE_NX)&&!mmu_identity(pa,false))return false;
 t[(va>>12)&511]=pa|flags|PAGE_PRESENT; invlpg(va); return true;
}
static bool clearing;
static uint64_t scan(uint64_t *t,unsigned level,uint64_t base) {
 unsigned i; uint64_t count=0;
 for(i=0;i<512;i++) { uint64_t e=__atomic_load_n(&t[i],__ATOMIC_RELAXED),va=base|((uint64_t)i<<(12+9*(level-1)));
  if(!(e&PAGE_PRESENT)) continue;
  if(level>1) { if(e&128) panic("unexpected huge page"); count+=scan((uint64_t *)(e&MASK),level-1,va); }
  else if((e&PAGE_DIRTY)&&sls_contains(va)) {
   if(clearing) { __atomic_fetch_and(&t[i],~PAGE_DIRTY,__ATOMIC_ACQ_REL); invlpg(va); count++; continue; }
   if(!sls_persist(va,(const void *)(e&MASK))) panic("SLS write/flush failed; dirty retained");
   uart_puts("SLS disk staged page "); uart_put_hex(va); uart_puts("\n");
   count++;
  }
 }
 return count;
}
uint64_t mmu_sls_commit(uint64_t *root) { uint64_t n; mfence(); sls_begin(); n=scan(root,4,0); if(!sls_commit()) panic("global SLS commit failed"); clearing=true; (void)scan(root,4,0); clearing=false; return n; }

bool mmu_map_device(uint64_t pa,uint64_t length) {
 uint64_t p,end;
 if(!length||pa>=0x100000000ULL||length>0x100000000ULL-pa) return false;
 end=(pa+length+4095)&~4095ULL;
 for(p=pa&~4095ULL;p<end;p+=4096)
  if(p>=PHYS_LIMIT&&!mmu_map(boot_pml4,p,p,PAGE_WRITE|PAGE_PCD|PAGE_PWT)) return false;
 return true;
}
