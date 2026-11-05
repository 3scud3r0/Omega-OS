#include "mmu.h"
#include "virtio_blk.h"
#include "uart.h"
/* Three-level 256-way CoW radix tree. Append-only allocation; an epoch is
 * published by one 512-byte superblock, alternated across two sectors. */
#define MAGIC 0x324f4d454741534cULL
#define BASE 0x40000000ULL
#define MAX_RESIDENT 4096U
#define MAX_LOGICAL 16777216ULL
static uint64_t resident[MAX_RESIDENT];
static unsigned count,crash_stage;
static uint64_t epoch,root_sector,root_hash,next_sector=8;
static uint64_t working[512] ALIGN_4K, directory[512] ALIGN_4K,leaf[512] ALIGN_4K;
static uint64_t checks[3][512] ALIGN_4K;
static uint64_t super[64] ALIGN_64;
static uint8_t scratch[4096] ALIGN_4K;
static uint64_t cursor;
static bool loaded,transaction,changed;
static uint64_t crc64(const void *p,size_t n) {
 const uint8_t *b=p; uint64_t c=0; size_t i; unsigned j;
 for(i=0;i<n;i++) { c^=(uint64_t)b[i]<<56; for(j=0;j<8;j++) c=(c<<1)^((c>>63)?0x42f0e1eba9ea3693ULL:0); } return c;
}
void sls_test_crash_after(unsigned stage) { crash_stage=stage; }
static void boundary(unsigned stage) {
 if(crash_stage!=stage) return;
 uart_puts("SLS crash-test boundary="); uart_put_dec(stage); uart_puts("\n");
 for(;;) __asm__ volatile("cli; hlt");
}
static bool block_valid(uint64_t s,uint64_t limit) { return s>=8&&!(s&7)&&s<=limit&&limit-s>=8; }
static bool read_checked(uint64_t s,uint64_t hash,void *out,uint64_t limit) {
 return block_valid(s,limit)&&virtio_blk_read_page(s,out)&&crc64(out,4096)==hash;
}
static bool validate(uint64_t sector,uint64_t hash,unsigned depth,uint64_t limit) {
 unsigned i;
 if(!read_checked(sector,hash,checks[depth],limit)) return false;
 for(i=0;i<256;i++) if(checks[depth][2*i]) {
  if(depth<2) { if(!validate(checks[depth][2*i],checks[depth][2*i+1],depth+1,limit)) return false; }
  else if(!read_checked(checks[depth][2*i],checks[depth][2*i+1],scratch,limit)) return false;
 }
 return true;
}
static void load(void) {
 unsigned slot; uint64_t best=0; bool nonempty=false;
 if(loaded) return;
 for(slot=0;slot<2;slot++) {
  uint64_t h;
  if(!virtio_blk_sector(slot,super,false)) panic("SLS superblock read");
  for(unsigned i=0;i<64;i++) if(super[i]) nonempty=true;
  h=super[5]; super[5]=0;
  if(super[0]!=MAGIC||!super[1]||super[1]<=best||crc64(super,512)!=h||super[4]>virtio_blk_capacity()) continue;
  if(!validate(super[2],super[3],0,super[4])) continue;
  best=super[1]; epoch=best; root_sector=super[2]; root_hash=super[3]; next_sector=super[4];
 }
 if(!best&&!nonempty) {
  for(unsigned oldslot=0;oldslot<2;oldslot++) {
   if(!virtio_blk_read_page(8+16*oldslot,scratch)) panic("SLS format probe");
   { uint64_t magic; memcpy(&magic,scratch,8); if(magic==0x314c53534147454dULL) panic("V1 disk needs offline migration; refusing overwrite"); }
  }
 }
 if(!best&&nonempty) panic("SLS V2 format/corruption: no valid global root; disk preserved");
 loaded=true; uart_puts("SLS global recovered epoch="); uart_put_dec(epoch); uart_puts("\n");
}
bool sls_contains(uint64_t va) { for(unsigned i=0;i<count;i++) if(resident[i]==va) return true; return false; }
bool sls_restore(uint64_t va) {
 uint64_t id=(va-BASE)/4096,s,h;
 load(); if(!sls_contains(va)||!root_sector) return false;
 s=root_sector; h=root_hash;
 for(unsigned depth=0;depth<3;depth++) {
  unsigned index=(unsigned)((id>>(16-8*depth))&255);
  if(!read_checked(s,h,checks[depth],next_sector)) panic("SLS tree integrity");
  s=checks[depth][2*index]; h=checks[depth][2*index+1]; if(!s) return false;
 }
 if(!read_checked(s,h,(void *)va,next_sector)) panic("SLS payload integrity");
 return true;
}
bool sls_register(uint64_t va) {
 if(va<BASE||(va&4095)||(va-BASE)/4096>=MAX_LOGICAL||count==MAX_RESIDENT||sls_contains(va)) panic("SLS registration bounds");
 resident[count++]=va; return sls_restore(va);
}
void sls_begin(void) {
 load(); if(transaction) panic("nested SLS transaction");
 memset(working,0,4096);
 if(root_sector&&!read_checked(root_sector,root_hash,working,next_sector)) panic("SLS root read");
 cursor=next_sector; transaction=true; changed=false;
}
static bool append(const void *data,uint64_t *s,uint64_t *h) {
 if(!block_valid(cursor,virtio_blk_capacity())) return false;
 *s=cursor; *h=crc64(data,4096); if(!virtio_blk_write_page(cursor,data)) return false;
 cursor+=8; return true;
}
bool sls_persist(uint64_t va,const void *data) {
 uint64_t id=(va-BASE)/4096,s,h; unsigned a=(unsigned)(id>>16),b=(unsigned)((id>>8)&255),c=(unsigned)(id&255);
 if(!transaction||!sls_contains(va)) return false;
 memset(directory,0,4096); memset(leaf,0,4096);
 if(working[2*a]&&!read_checked(working[2*a],working[2*a+1],directory,cursor)) return false;
 if(directory[2*b]&&!read_checked(directory[2*b],directory[2*b+1],leaf,cursor)) return false;
 if(!append(data,&s,&h)) return false;
 boundary(1); leaf[2*c]=s; leaf[2*c+1]=h;
 if(!append(leaf,&s,&h)) return false;
 directory[2*b]=s; directory[2*b+1]=h;
 if(!append(directory,&s,&h)) return false;
 working[2*a]=s; working[2*a+1]=h; changed=true; return true;
}
bool sls_commit(void) {
 uint64_t s,h;
 if(!transaction) return false;
 if(!changed) { transaction=false; return true; }
 if(epoch==~0ULL||!append(working,&s,&h)||!virtio_blk_flush()) return false;
 boundary(2); memset(super,0,512); super[0]=MAGIC; super[1]=epoch+1;
 super[2]=s; super[3]=h; super[4]=cursor; super[5]=crc64(super,512);
 if(!virtio_blk_sector((epoch+1)&1,super,true)) return false;
 boundary(3); if(!virtio_blk_flush()) return false; boundary(4);
 epoch++; root_sector=s; root_hash=h; next_sector=cursor; transaction=false;
 uart_puts("SLS global atomic epoch="); uart_put_dec(epoch); uart_puts("\n"); return true;
}
