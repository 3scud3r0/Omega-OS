#include "udp.h"
#include "net.h"
#include "tunnel.h"
#include "mmu.h"
#include "uart.h"
#include "crypto/monocypher.h"
#define PEER_MAGIC 0x33524545504f4dULL
static tunnel_t channel;
static uint8_t outgoing[4096] ALIGN_4K,incoming[4096] ALIGN_4K;
static uint8_t plain[1100],app[1100],hello[104],out_hash[32],assembly_hash[32];
static uint64_t *state=(uint64_t *)0x40003000;
static uint8_t *replica=(uint8_t *)0x40004000;
static void entropy(uint8_t *p,size_t n){
 uint32_t a,b,c,d;__asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1),"c"(0));
 if(!(c&(1U<<30)))panic("P2P requires RDRAND entropy; no timestamp fallback");
 while(n){uint64_t value=0;uint8_t ok=0;for(unsigned retry=0;retry<64&&!ok;retry++)__asm__ volatile("rdrand %0; setc %1":"=r"(value),"=qm"(ok));if(!ok)panic("hardware entropy unavailable");size_t count=n<8?n:8;memcpy(p,&value,count);p+=count;n-=count;crypto_wipe(&value,8);}
}
static void map_page(uint64_t va){uint64_t p=pmm_alloc_frame();if(!p||!mmu_map(boot_pml4,va,p,PAGE_WRITE))panic("secure peer SLS mapping");(void)sls_register(va);}
static bool send_secure(const uint8_t *body,size_t n){uint8_t *wire=udp_tx_payload();size_t bytes=tunnel_seal(&channel,wire,body,n);return bytes&&udp_send(wire,bytes);}
void net_test(void){
 uint8_t random[48],pin[32],local_key[32];uint64_t source_epoch,assembly_epoch=0,last_hello=0,last_data=0,start,finished=0;unsigned mask=0,next_chunk=0;
 bool received=false,acked=false;
 if(!net_init()){uart_puts("NET optional device absent\n");return;}
 if(!net_config.configured)panic("secure UDP network requires trusted initrd configuration");
 map_page((uint64_t)state);map_page((uint64_t)replica);
 crypto_blake2b(pin,32,net_config.peer_key,32);crypto_x25519_public_key(local_key,net_config.secret);
 if(state[0]==PEER_MAGIC){if(crypto_verify32((uint8_t *)state+104,pin)||crypto_verify32((uint8_t *)state+168,local_key)||state[17]!=net_config.local_ip)panic("peer identity/config changed");}
 else {memset(state,0,4096);state[0]=PEER_MAGIC;memcpy((uint8_t *)state+104,pin,32);memcpy((uint8_t *)state+168,local_key,32);state[17]=net_config.local_ip;}
 if(state[2]==~0ULL)panic("network boot counter exhausted");
 state[2]++;if(!mmu_sls_commit(boot_pml4))panic("durable network boot counter");
 entropy(random,sizeof random);
 if(!tunnel_init(&channel,net_config.secret,net_config.peer_key,random,state[2],state[3],(uint8_t *)state+32,net_config.local_ip,net_config.peer_ip))panic("X25519 peer configuration");
 crypto_wipe(net_config.secret,32);crypto_wipe(random,sizeof random);
 memcpy(outgoing,(void *)0x40005000,4096);crypto_blake2b(out_hash,32,outgoing,4096);source_epoch=sls_epoch();tunnel_hello(&channel,hello);
 uart_puts("P2P authenticated X25519 / ChaCha20-Poly1305 over IPv4/UDP starting\n");start=rdtsc();
 while(rdtsc()-start<60000000000ULL){
  const uint8_t *payload;size_t bytes;
  if(rdtsc()-last_hello>20000000ULL){(void)udp_send(hello,sizeof hello);last_hello=rdtsc();}
  if(channel.established&&!acked&&rdtsc()-last_data>10000000ULL){
   memset(app,0,64);app[0]=1;memcpy(app+16,&source_epoch,8);memcpy(app+24,out_hash,32);
   uint16_t offset=(uint16_t)(next_chunk*1024),length=1024;memcpy(app+56,&offset,2);memcpy(app+58,&length,2);memcpy(app+64,outgoing+offset,1024);
   (void)send_secure(app,1088);next_chunk=(next_chunk+1)%4;last_data=rdtsc();
  }
  if(udp_receive(&payload,&bytes)){
   bool changed=false;
   if(bytes==104&&payload[8]==1){
    if(tunnel_accept(&channel,payload,bytes,&changed)&&changed){state[3]=channel.peer_boot;memcpy((uint8_t *)state+32,channel.saved_hello,32);if(!mmu_sls_commit(boot_pml4))panic("peer boot replay floor");uart_puts("P2P peer authenticated; durable replay floor installed\n");}
   }else{
    size_t length;
    if(tunnel_open(&channel,plain,&length,payload,bytes)&&length>=64){
     if(plain[0]==2&&!crypto_verify32(plain+24,out_hash))acked=true;
     else if(plain[0]==1&&length==1088){
      uint64_t epoch;uint16_t offset,size;memcpy(&epoch,plain+16,8);memcpy(&offset,plain+56,2);memcpy(&size,plain+58,2);
      if(size==1024&&offset<=3072&&!(offset%1024)&&(epoch>state[8]||(epoch==state[8]&&!crypto_verify32(plain+24,(uint8_t *)state+72)))){
       if(received&&!crypto_verify32(plain+24,assembly_hash)){memset(app,0,64);app[0]=2;memcpy(app+24,assembly_hash,32);(void)send_secure(app,64);}
       else{
        if(!mask||epoch>assembly_epoch){mask=0;assembly_epoch=epoch;memcpy(assembly_hash,plain+24,32);}
        if(epoch==assembly_epoch&&!crypto_verify32(assembly_hash,plain+24)){
         memcpy(incoming+offset,plain+64,1024);mask|=1U<<(offset/1024);
         if(mask==15){uint8_t digest[32];crypto_blake2b(digest,32,incoming,4096);if(crypto_verify32(digest,assembly_hash))panic("authenticated snapshot assembly mismatch");
          memcpy(replica,incoming,4096);state[8]=epoch;memcpy((uint8_t *)state+72,digest,32);if(!mmu_sls_commit(boot_pml4))panic("replicated page commit");received=true;
          memset(app,0,64);app[0]=2;memcpy(app+24,digest,32);(void)send_secure(app,64);
         }
        }
       }
      }
     }
    }
   }
   udp_release();
  }
  if(received&&acked){if(!finished)finished=rdtsc();if(rdtsc()-finished>200000000ULL)break;}
  cpu_pause();
 }
 if(!received||!acked)panic("authenticated UDP peer timeout");
 uart_puts("P2P rejected replay=");uart_put_dec(channel.replay_drops);uart_puts(" authentication=");uart_put_dec(channel.auth_drops);uart_puts("\n");
 uart_puts("TEST routed encrypted UDP + authenticated 4096-byte SLS replication PASS\n");
 crypto_wipe(&channel,sizeof channel);
}
