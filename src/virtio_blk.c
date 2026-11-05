#include "virtio_blk.h"
#include "pci.h"
#include "uart.h"
/* Legacy vring: avail immediately follows descriptors, used is page-aligned.
 * Padding avail to a page would VIOLATE the legacy queue layout. */
static uint8_t queue_memory[32768] ALIGN_4K __attribute__((section(".bss.dma")));
static vring_desc *descriptors;
static vring_avail *available;
static vring_used *used;
static uint16_t queue_size;
static struct { uint32_t type,reserved; uint64_t sector; } request ALIGN_64;
static uint8_t dma_page[4096] ALIGN_4K __attribute__((section(".bss.dma")));
static uint8_t status ALIGN_64;
static uint16_t io,last_used;
static bool inflight,failed,write_operation;
static void *read_target;
static uint64_t capacity;
static uint32_t transfer_length;
static bool finish(void) {
 uint64_t start=rdtsc(); bool ok=false;
 while(!virtio_blk_poll(&ok)) {
  if(rdtsc()-start>10000000000ULL) { failed=true; panic("VirtIO timeout: DMA buffers retained"); }
  cpu_pause();
 }
 return ok;
}
void virtio_blk_init(void) {
 pci_device_t d=pci_find_device(0x1af4,0x1001); uint32_t bar,features;
 if(!d.found) {
  d=pci_find_device(0x1af4,0x1042);
  panic(d.found?"modern-only VirtIO unsupported: use transitional device":"VirtIO PCI device absent");
 }
 bar=pci_read(d,0x10);
 if(!(bar&1U)||(bar&~3U)>65500U) panic("VirtIO invalid I/O BAR");
 io=(uint16_t)(bar&~3U); pci_enable_io_dma(d);
 outb(io+18,0); outb(io+18,1); outb(io+18,3);
 features=inl(io);
 if(!(features&(1U<<9))) panic("VirtIO FLUSH required for durability");
 outl(io+4,1U<<9); outw(io+14,0);
 queue_size=inw(io+12);
 uart_puts("VirtIO IO="); uart_put_hex(io); uart_puts(" qsize="); uart_put_dec(queue_size); uart_puts(" PFN="); uart_put_hex(inl(io+8)); uart_puts("\n");
 if((queue_size<128||queue_size>1024||(queue_size&(queue_size-1)))||inl(io+8)!=0) panic("VirtIO queue size unsupported");
 memset(queue_memory,0,sizeof queue_memory);
 descriptors=(vring_desc *)queue_memory;
 available=(vring_avail *)(queue_memory+16U*queue_size);
 used=(vring_used *)(queue_memory+((16U*queue_size+6U+2U*queue_size+4095U)&~4095U));
 outl(io+8,(uint32_t)((uint64_t)queue_memory>>12));
 capacity=(uint64_t)inl(io+20)|((uint64_t)inl(io+24)<<32);
 if(capacity<1024) panic("VirtIO disk too small");
 outb(io+18,7);
 if(inb(io+18)&128) panic("VirtIO device rejected initialization");
 uart_puts("PCI VirtIO legacy detected; DMA queue="); uart_put_dec(queue_size); uart_puts("; FLUSH negotiated\n");
}
static bool submit(uint32_t type,uint64_t sector,const void *page,uint32_t length) {
 vring_desc *d=descriptors; uint16_t a;
 if(!io||failed||inflight||sector>capacity||capacity-sector<length/512) return false;
 if(length>4096||length%512||(!page&&type!=4)) return false;
 transfer_length=length; request.type=type; request.reserved=0; request.sector=sector;
 status=0xff; write_operation=type!=0; read_target=(void *)page;
 if(type==1) memcpy(dma_page,page,length);
 d[0].addr=(uint64_t)&request; d[0].len=16; d[0].flags=1; d[0].next=type==4?2:1;
 d[1].addr=(uint64_t)dma_page; d[1].len=length; d[1].flags=(uint16_t)(1|(type==0?2:0)); d[1].next=2;
 d[2].addr=(uint64_t)&status; d[2].len=1; d[2].flags=2; d[2].next=0;
 a=available->idx; ((uint16_t *)((uint8_t *)available+4))[a%queue_size]=0;
 inflight=true;
 __atomic_store_n(&available->idx,(uint16_t)(a+1),__ATOMIC_RELEASE);
 mfence(); outw(io+16,0); return true;
}
bool virtio_blk_submit(uint64_t sector,const void *page,bool write) { return submit(write?1U:0U,sector,page,4096); }
bool virtio_blk_poll(bool *ok) {
 uint16_t u; uint8_t s;
 if(!inflight) { *ok=false; return true; }
 u=__atomic_load_n(&used->idx,__ATOMIC_ACQUIRE);
 if(u==last_used) return false;
 if((uint16_t)(u-last_used)!=1 || ((vring_used_elem *)((uint8_t *)used+4))[last_used%queue_size].id!=0) panic("VirtIO corrupt used ring");
 s=__atomic_load_n(&status,__ATOMIC_ACQUIRE); *ok=s==0;
 if(*ok&&!write_operation) memcpy(read_target,dma_page,transfer_length);
 last_used=u; inflight=false; (void)inb(io+19); return true;
}
bool virtio_blk_write_page(uint64_t sector,const void *page) { return virtio_blk_submit(sector,page,true)&&finish(); }
bool virtio_blk_read_page(uint64_t sector,void *page) { return virtio_blk_submit(sector,page,false)&&finish(); }
bool virtio_blk_flush(void) { return submit(4,0,0,0)&&finish(); }

uint64_t virtio_blk_capacity(void) { return capacity; }
bool virtio_blk_sector(uint64_t sector,void *data,bool write) { return submit(write?1U:0U,sector,data,512)&&finish(); }
