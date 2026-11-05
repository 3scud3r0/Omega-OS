#ifndef OMEGA_PCI_H
#define OMEGA_PCI_H
#include "types.h"
typedef struct { uint8_t bus,slot,function; bool found; } pci_device_t;
pci_device_t pci_find_device(uint16_t vendor,uint16_t device);
uint32_t pci_read(pci_device_t d,uint8_t offset);
void pci_enable_io_dma(pci_device_t d);
#endif
