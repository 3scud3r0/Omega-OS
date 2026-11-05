#include "mmu.h"
#include "virtio_blk.h"
#include "uart.h"
/* Per-page, two-slot copy-on-write journal. No disk pointer is trusted without
 * checksums. Data is flushed before publishing metadata; old slot survives.
 * This is NOT a cross-page atomic transaction or cryptographic authentication. */
#define SLS_MAGIC 0x314c53534147454dULL
#define MAX_PAGES 4
static struct { uint64_t va,generation; } pages[MAX_PAGES];
static unsigned page_count;
static unsigned crash_boundary;
void sls_test_crash_after(unsigned boundary) { crash_boundary=boundary; }
static void crash_test_boundary(unsigned boundary) {
 if(crash_boundary!=boundary) return;
 uart_puts("SLS crash-test boundary="); uart_put_dec(boundary); uart_puts("\n");
 /* Deliberate test pause. Host SIGKILL ends the emulator at this I/O boundary. */
 for(;;) __asm__ volatile("cli; hlt");
}
static uint64_t metadata[512] ALIGN_4K;
static uint8_t scratch[4096] ALIGN_4K;
static uint8_t recovered[4096] ALIGN_4K;
static uint64_t crc64(const void *p,size_t n) {
 const uint8_t *b=p; uint64_t crc=0; size_t i; unsigned j;
 for(i=0;i<n;i++) { crc^=(uint64_t)b[i]<<56;
  for(j=0;j<8;j++) crc=(crc<<1)^((crc>>63)?0x42f0e1eba9ea3693ULL:0);
 }
 return crc;
}
static unsigned index_of(uint64_t va) {
 unsigned i; for(i=0;i<page_count;i++) if(pages[i].va==va) return i; return MAX_PAGES;
}
bool sls_contains(uint64_t va) { return index_of(va)!=MAX_PAGES; }
static uint64_t slot_sector(unsigned i,unsigned slot) { return 8ULL+(uint64_t)i*32+(uint64_t)slot*16; }
bool sls_restore(uint64_t va) {
 unsigned i=index_of(va),slot; uint64_t best=0; bool nonempty=false;
 if(i==MAX_PAGES) return false;
 for(slot=0;slot<2;slot++) {
  uint64_t base=slot_sector(i,slot),sum;
  if(!virtio_blk_read_page(base,metadata)) panic("SLS metadata read failed");
  for(unsigned j=0;j<512;j++) if(metadata[j]) nonempty=true;
  sum=metadata[4]; metadata[4]=0;
  if(metadata[0]!=SLS_MAGIC||metadata[2]!=va||!metadata[1]||metadata[1]<=best||crc64(metadata,4096)!=sum) continue;
  if(!virtio_blk_read_page(base+8,scratch)) panic("SLS data read failed");
  if(crc64(scratch,4096)!=metadata[3]) continue;
  best=metadata[1]; memcpy(recovered,scratch,4096);
 }
 if(!best) { if(nonempty) panic("SLS no valid checkpoint; refusing reinitialization"); return false; }
 memcpy((void *)va,recovered,4096); pages[i].generation=best;
 uart_puts("SLS recovered disk generation "); uart_put_dec(best); uart_puts("\n"); return true;
}
bool sls_register(uint64_t va) {
 if((va&4095)||page_count==MAX_PAGES||sls_contains(va)) panic("invalid SLS registration");
 pages[page_count++].va=va; return sls_restore(va);
}
bool sls_persist(uint64_t va,const void *data) {
 unsigned i=index_of(va); uint64_t gen,base;
 if(i==MAX_PAGES||pages[i].generation==~0ULL) return false;
 gen=pages[i].generation+1; base=slot_sector(i,(unsigned)(gen&1));
 /* Source must remain quiescent for this transaction. DMA uses a bounce page. */
 memcpy(scratch,data,4096); memset(metadata,0,4096);
 metadata[0]=SLS_MAGIC; metadata[1]=gen; metadata[2]=va; metadata[3]=crc64(scratch,4096);
 metadata[4]=crc64(metadata,4096);
 if(!virtio_blk_write_page(base+8,scratch)) return false;
 crash_test_boundary(1);
 if(!virtio_blk_flush()) return false;
 crash_test_boundary(2);
 if(!virtio_blk_write_page(base,metadata)) return false;
 crash_test_boundary(3);
 if(!virtio_blk_flush()) return false;
 crash_test_boundary(4);
 pages[i].generation=gen; return true;
}
