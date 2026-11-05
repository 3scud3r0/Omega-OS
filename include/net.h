#ifndef OMEGA_NET_H
#define OMEGA_NET_H
#include "types.h"
bool net_init(void);
uint64_t net_mac(void);
bool net_send_page(const void *physical_page,size_t length);
uint8_t *net_tx_buffer(void);
bool net_send(size_t length);
bool net_receive(const uint8_t **frame,size_t *length);
void net_release(void);
void net_test(void);
#endif
