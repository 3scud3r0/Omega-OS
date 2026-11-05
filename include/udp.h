#ifndef OMEGA_UDP_H
#define OMEGA_UDP_H
#include "types.h"
typedef struct {uint32_t local_ip,peer_ip,gateway,mask;uint16_t local_port,peer_port;uint8_t secret[32],peer_key[32];bool configured;} net_config_t;
extern net_config_t net_config;
void net_boot_config(uint32_t info);
uint8_t *udp_tx_payload(void);
bool udp_send(const uint8_t *body,size_t n);
bool udp_receive(const uint8_t **body,size_t *n);
void udp_release(void);
#endif
