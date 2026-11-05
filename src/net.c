#include "net.h"
#include "pci.h"
#include "virtio_blk.h"
#include "mmu.h"
#include "uart.h"
#define MAXQ 1024U
#define RX_BUFFERS 8U
#define FRAME_MAX 1514U
static uint8_t queue_mem[2][32768] ALIGN_4K;
static uint8_t rx[RX_BUFFERS][1536] ALIGN_64;
static uint8_t tx[1536] ALIGN_64;
static struct { vring_desc *desc; uint16_t *avail; uint8_t *used; uint16_t size,last; } q[2];
static uint16_t io,loan;
static bool loaned;
static uint8_t mac[6];
static void offer(unsigned queue,uint16_t id) {
 uint16_t n=q[queue].avail[1]; q[queue].avail[2+n%q[queue].size]=id;
 __atomic_store_n(&q[queue].avail[1],(uint16_t)(n+1),__ATOMIC_RELEASE);
 mfence(); outw(io+16,(uint16_t)queue);
}
bool net_init(void) {
 pci_device_t d=pci_find_device(0x1af4,0x1000); uint32_t bar,features; unsigned i;
 if(io) return true;
 if(!d.found) {
  if(pci_find_device(0x1af4,0x1041).found) panic("modern-only VirtIO-Net unsupported");
  return false;
 }
 bar=pci_read(d,16); if(!(bar&1)||(bar&~3U)>65500) panic("VirtIO-Net BAR");
 pci_enable_io_dma(d); io=(uint16_t)(bar&~3U);
 outb(io+18,0); outb(io+18,1); outb(io+18,3);
 features=inl(io); if(!(features&(1U<<5))) panic("VirtIO-Net MAC feature required");
 outl(io+4,1U<<5);
 for(i=0;i<6;i++) mac[i]=inb((uint16_t)(io+20+i));
 for(i=0;i<2;i++) {
  uint16_t size; outw(io+14,(uint16_t)i); size=inw(io+12);
  if(size<RX_BUFFERS||size>MAXQ||(size&(size-1))||inl(io+8)) panic("VirtIO-Net queue");
  q[i].size=size; q[i].desc=(vring_desc *)queue_mem[i];
  q[i].avail=(uint16_t *)(queue_mem[i]+16U*size);
  q[i].used=queue_mem[i]+((16U*size+6U+2U*size+4095U)&~4095U);
  outl(io+8,(uint32_t)((uint64_t)queue_mem[i]>>12));
 }
 outb(io+18,7);
 for(i=0;i<RX_BUFFERS;i++) { q[0].desc[i].addr=(uint64_t)rx[i]; q[0].desc[i].len=1536; q[0].desc[i].flags=2; offer(0,(uint16_t)i); }
 uart_puts("VirtIO-Net RX/TX DMA ready MAC="); uart_put_hex(mac[5]); uart_puts("\n"); return true;
}
uint8_t *net_tx_buffer(void) { return tx+10; }
uint64_t net_mac(void) { uint64_t v=0; if(io) memcpy(&v,mac,6); return v; }
bool net_send(size_t length) { return net_send_page(tx+10,length); }
bool net_send_page(const void *physical_page,size_t length) {
 uint64_t start=rdtsc(); uint16_t target;
 if(!io||length<14||length>FRAME_MAX) return false;
 memset(tx,0,10); q[1].desc[0].addr=(uint64_t)tx; q[1].desc[0].len=10; q[1].desc[0].flags=1; q[1].desc[0].next=1;
 q[1].desc[1].addr=(uint64_t)physical_page; q[1].desc[1].len=(uint32_t)length; q[1].desc[1].flags=0;
 target=(uint16_t)(q[1].last+1); offer(1,0);
 while(__atomic_load_n((uint16_t *)(q[1].used+2),__ATOMIC_ACQUIRE)!=target) {
  if(rdtsc()-start>10000000000ULL) return false;
  cpu_pause();
 }
 { vring_used_elem *u=(vring_used_elem *)(q[1].used+4); if(u[q[1].last%q[1].size].id!=0) panic("Net TX completion"); }
 q[1].last=target; (void)inb(io+19); return true;
}
bool net_receive(const uint8_t **frame,size_t *length) {
 uint16_t n; vring_used_elem element;
 if(loaned||!io) return false;
 n=__atomic_load_n((uint16_t *)(q[0].used+2),__ATOMIC_ACQUIRE); if(n==q[0].last) return false;
 if((uint16_t)(n-q[0].last)>RX_BUFFERS) panic("Net used ring overrun");
 element=((vring_used_elem *)(q[0].used+4))[q[0].last%q[0].size];
 if(element.id>=RX_BUFFERS||element.len<24||element.len>1536) panic("Net malformed RX completion");
 loan=(uint16_t)element.id; loaned=true; *frame=rx[loan]+10; *length=element.len-10; return true;
}
void net_release(void) {
 if(!loaned) panic("Net release without loan");
 q[0].last++; loaned=false; offer(0,loan); (void)inb(io+19);
}
static bool packet(uint8_t kind,uint64_t nonce) {
 uint8_t *p=net_tx_buffer(); memset(p,0,64);
 memset(p,255,6); memcpy(p+6,mac,6); p[12]=0x88; p[13]=0xb5; p[14]=kind;
 memcpy(p+16,&nonce,8); return net_send(64);
}
void net_test(void) {
 uint64_t start,last=0,nonce=0x4f4d454741000000ULL; bool received=false,acked=false; uint8_t *page;
 if(!net_init()) { uart_puts("NET optional device absent\n"); return; }
 nonce|=mac[5];
 { uint64_t phys=pmm_alloc_frame(); if(!phys||!mmu_map(boot_pml4,0x40003000,phys,PAGE_WRITE)) panic("network SLS page"); }
 page=(uint8_t *)0x40003000; (void)sls_register((uint64_t)page);
 start=rdtsc();
 while(rdtsc()-start<20000000000ULL) {
  const uint8_t *p; size_t length;
  if(rdtsc()-last>10000000ULL) { if(!packet(1,nonce)) panic("network TX"); last=rdtsc(); }
  if(net_receive(&p,&length)) {
   if(length>=24&&p[12]==0x88&&p[13]==0xb5) {
    uint64_t peer; memcpy(&peer,p+16,8);
    if(p[14]==1&&peer!=nonce) {
     /* Copy into durable state is intentional; RX itself remains zero-copy. */
     memset(page,0,4096); memcpy(page,"OMEGA-PEER-V2",13); memcpy(page+16,&peer,8); received=true;
     if(!packet(2,peer)) panic("network ACK");
    } else if(p[14]==2&&peer==nonce) acked=true;
   }
   net_release();
  }
  if(received&&acked) break;
  cpu_pause();
 }
 if(!received||!acked) panic("network peer timeout");
 if(!mmu_sls_commit(boot_pml4)) panic("peer persistence");
 uart_puts("TEST VirtIO-Net two-node Ethernet + peer durable commit PASS\n");
}
