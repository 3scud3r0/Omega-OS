#ifndef OMEGA_TUNNEL_H
#define OMEGA_TUNNEL_H
#include "types.h"
typedef struct {
 uint8_t auth[32],secret[32],public_key[32],nonce[16],peer_public[32],peer_nonce[16],session[16],tx[32],rx[32],saved_hello[32];
 uint64_t boot,peer_boot,min_peer,sequence,highest,window,replay_drops,auth_drops;
 uint32_t local_ip,peer_ip; bool established;
} tunnel_t;
bool tunnel_init(tunnel_t *t,const uint8_t identity_secret[32],const uint8_t peer_key[32],const uint8_t entropy[48],uint64_t boot,uint64_t peer_boot,const uint8_t saved[32],uint32_t local_ip,uint32_t peer_ip);
void tunnel_hello(tunnel_t *t,uint8_t out[104]);
bool tunnel_accept(tunnel_t *t,const uint8_t *hello,size_t size,bool *changed);
size_t tunnel_seal(tunnel_t *t,uint8_t *out,const uint8_t *plain,size_t n);
bool tunnel_open(tunnel_t *t,uint8_t *plain,size_t *n,const uint8_t *packet,size_t size);
#endif
