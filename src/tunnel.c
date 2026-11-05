#include "tunnel.h"
#include "crypto/monocypher.h"
static const uint8_t magic[8]="OMPNET3";
static bool zero(const uint8_t *p){uint8_t sum=0;for(unsigned i=0;i<32;i++)sum|=p[i];return sum==0;}
static bool eqmagic(const uint8_t *p){for(unsigned i=0;i<8;i++)if(p[i]!=magic[i])return false;return true;}
bool tunnel_init(tunnel_t *t,const uint8_t identity_secret[32],const uint8_t peer_key[32],const uint8_t entropy[48],uint64_t boot,uint64_t peer_boot,const uint8_t saved[32],uint32_t local_ip,uint32_t peer_ip){
 uint8_t shared[32];static const uint8_t domain[16]="OM-AUTH-V3";
 if(!boot||local_ip==peer_ip)return false;
 memset(t,0,sizeof *t);crypto_x25519(shared,identity_secret,peer_key);if(zero(shared))return false;
 crypto_blake2b_keyed(t->auth,32,shared,32,domain,sizeof domain);crypto_wipe(shared,32);
 memcpy(t->secret,entropy,32);memcpy(t->nonce,entropy+32,16);crypto_x25519_public_key(t->public_key,t->secret);
 t->boot=boot;t->min_peer=peer_boot;t->local_ip=local_ip;t->peer_ip=peer_ip;memcpy(t->saved_hello,saved,32);return true;
}
void tunnel_hello(tunnel_t *t,uint8_t out[104]){
 memset(out,0,104);memcpy(out,magic,8);out[8]=1;out[9]=3;memcpy(out+12,&t->local_ip,4);memcpy(out+16,&t->boot,8);
 memcpy(out+24,t->public_key,32);memcpy(out+56,t->nonce,16);crypto_blake2b_keyed(out+72,32,t->auth,32,out,72);
}
bool tunnel_accept(tunnel_t *t,const uint8_t *hello,size_t size,bool *changed){
 uint8_t mac[32],fingerprint[32],shared[32],transcript[168],keys[64];uint64_t epoch;uint32_t ip;bool lower=t->local_ip<t->peer_ip;
 *changed=false;if(size!=104||!eqmagic(hello)||hello[8]!=1||hello[9]!=3)return false;
 crypto_blake2b_keyed(mac,32,t->auth,32,hello,72);if(crypto_verify32(mac,hello+72)){t->auth_drops++;return false;}
 memcpy(&epoch,hello+16,8);memcpy(&ip,hello+12,4);if(ip!=t->peer_ip||!epoch||epoch<t->min_peer){t->replay_drops++;return false;}
 crypto_blake2b(fingerprint,32,hello,72);
 if(epoch==t->min_peer&&!zero(t->saved_hello)&&crypto_verify32(fingerprint,t->saved_hello)){t->replay_drops++;return false;}
 if(t->established&&epoch==t->peer_boot){if(crypto_verify32(hello+24,t->peer_public)||crypto_verify16(hello+56,t->peer_nonce)){t->replay_drops++;return false;}return true;}
 if(t->established&&epoch<t->peer_boot){t->replay_drops++;return false;}
 crypto_x25519(shared,t->secret,hello+24);if(zero(shared))return false;
 memset(transcript,0,sizeof transcript);memcpy(transcript,"OM-SESSION-V3",13);memcpy(transcript+16,shared,32);
 memcpy(transcript+(lower?48:108),t->public_key,32);memcpy(transcript+(lower?80:140),t->nonce,16);memcpy(transcript+(lower?96:156),&t->boot,8);memcpy(transcript+(lower?104:164),&t->local_ip,4);
 memcpy(transcript+(lower?108:48),hello+24,32);memcpy(transcript+(lower?140:80),hello+56,16);memcpy(transcript+(lower?156:96),&epoch,8);memcpy(transcript+(lower?164:104),&ip,4);
 crypto_blake2b_keyed(keys,64,t->auth,32,transcript,sizeof transcript);memcpy(t->tx,keys+(lower?0:32),32);memcpy(t->rx,keys+(lower?32:0),32);
 crypto_blake2b_keyed(t->session,16,t->auth,32,transcript,sizeof transcript);
 memcpy(t->peer_public,hello+24,32);memcpy(t->peer_nonce,hello+56,16);memcpy(t->saved_hello,fingerprint,32);
 t->peer_boot=epoch;t->min_peer=epoch;t->sequence=1;t->highest=0;t->window=0;t->established=true;*changed=true;
 crypto_wipe(shared,32);crypto_wipe(keys,64);crypto_wipe(transcript,sizeof transcript);return true;
}
size_t tunnel_seal(tunnel_t *t,uint8_t *out,const uint8_t *plain,size_t n){
 uint8_t nonce[12]={0};crypto_aead_ctx aead;uint16_t length=(uint16_t)n;
 if(!t->established||n>1100||t->sequence==~0ULL)return 0;
 memset(out,0,40);memcpy(out,magic,8);out[8]=2;out[9]=3;memcpy(out+10,&length,2);memcpy(out+16,&t->sequence,8);memcpy(out+24,t->session,16);
 memcpy(nonce+4,&t->sequence,8);t->sequence++;
 crypto_aead_init_ietf(&aead,t->tx,nonce);crypto_aead_write(&aead,out+40,out+40+n,out,40,plain,n);crypto_wipe(&aead,sizeof aead);return n+56;
}
bool tunnel_open(tunnel_t *t,uint8_t *plain,size_t *n,const uint8_t *p,size_t size){
 uint64_t seq,delta;uint16_t length;uint8_t nonce[12]={0};crypto_aead_ctx aead;
 if(!t->established||size<56||!eqmagic(p)||p[8]!=2||p[9]!=3||crypto_verify16(p+24,t->session))return false;
 memcpy(&seq,p+16,8);memcpy(&length,p+10,2);if(!seq||length>1100||size!=56U+length)return false;
 if(seq<=t->highest){delta=t->highest-seq;if(delta>=64||(t->window&(1ULL<<delta))){t->replay_drops++;return false;}}
 memcpy(nonce+4,&seq,8);crypto_aead_init_ietf(&aead,t->rx,nonce);
 if(crypto_aead_read(&aead,plain,p+40+length,p,40,p+40,length)){crypto_wipe(&aead,sizeof aead);t->auth_drops++;return false;}
 crypto_wipe(&aead,sizeof aead);
 if(seq>t->highest){delta=seq-t->highest;t->window=delta>=64?1:(t->window<<delta)|1;t->highest=seq;}else t->window|=1ULL<<(t->highest-seq);
 *n=length;return true;
}
