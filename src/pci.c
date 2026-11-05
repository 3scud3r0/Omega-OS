#include "pci.h"
static uint32_t address(pci_device_t d,uint8_t o) {
 return 0x80000000U|((uint32_t)d.bus<<16)|((uint32_t)d.slot<<11)|((uint32_t)d.function<<8)|(o&0xfcU);
}
uint32_t pci_read(pci_device_t d,uint8_t o) { outl(0xcf8,address(d,o)); return inl(0xcfc); }
void pci_enable_io_dma(pci_device_t d) {
 uint16_t cmd=(uint16_t)pci_read(d,4);
 outl(0xcf8,address(d,4)); outw(0xcfc,(uint16_t)(cmd|5U|0x400U));
}
pci_device_t pci_find_device(uint16_t vendor,uint16_t device) {
 unsigned b,s,f; pci_device_t d={0,0,0,false};
 for(b=0;b<256;b++) for(s=0;s<32;s++) for(f=0;f<8;f++) {
  uint32_t id; d.bus=(uint8_t)b; d.slot=(uint8_t)s; d.function=(uint8_t)f;
  id=pci_read(d,0);
  if(id==((uint32_t)device<<16|vendor)) { d.found=true; return d; }
  if(!f && ((id&0xffffU)==0xffffU || !(pci_read(d,0x0c)&0x800000U))) break;
 }
 return d;
}
