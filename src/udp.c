#include "udp.h"
#include "net.h"
#include "mmu.h"
#include "crypto/monocypher.h"
#include "uart.h"
net_config_t net_config;
static uint8_t next_mac[6];static bool resolved;static uint64_t last_arp;
static uint16_t be16(const uint8_t *p){return (uint16_t)((unsigned)p[0]*256+p[1]);}
static uint32_t be32(const uint8_t *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void put16(uint8_t *p,uint16_t v){p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v;}
static void put32(uint8_t *p,uint32_t v){p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v;}
static uint32_t sum(const uint8_t *p,size_t n,uint32_t value){while(n>1){value+=be16(p);p+=2;n-=2;}if(n)value+=(uint32_t)*p<<8;return value;}
static uint16_t finish(uint32_t value){while(value>>16)value=(value&65535)+(value>>16);return (uint16_t)~value;}
static uint32_t next_hop(void){return (net_config.local_ip&net_config.mask)==(net_config.peer_ip&net_config.mask)?net_config.peer_ip:net_config.gateway;}
void net_boot_config(uint32_t info){
 const uint32_t *mb=(const uint32_t *)(uint64_t)info;uint32_t start,end;uint8_t digest[32];static const uint8_t magic[8]="OMNETV3";
 if(!(mb[0]&8)||mb[5]!=1)return;
 if(mb[6]>PHYS_LIMIT-16)panic("network module descriptor");
 const uint32_t *module=(const uint32_t *)(uint64_t)mb[6];start=module[0];end=module[1];
 if(start>=end||end>PHYS_LIMIT||end-start!=128)panic("network module bounds");
 uint8_t *p=(uint8_t *)(uint64_t)start;
 for(unsigned i=0;i<8;i++)if(p[i]!=magic[i])panic("network config domain");
 crypto_blake2b(digest,32,p,96);if(crypto_verify32(digest,p+96))panic("network config checksum");
 memcpy(&net_config.local_ip,p+12,4);memcpy(&net_config.peer_ip,p+16,4);memcpy(&net_config.gateway,p+20,4);memcpy(&net_config.mask,p+24,4);
 memcpy(&net_config.local_port,p+28,2);memcpy(&net_config.peer_port,p+30,2);memcpy(net_config.secret,p+32,32);memcpy(net_config.peer_key,p+64,32);
 crypto_wipe(p+32,32);
 if(!net_config.local_ip||!net_config.peer_ip||!net_config.local_port||!net_config.peer_port)panic("network static configuration");
 net_config.configured=true;
}
static bool arp(uint16_t op,const uint8_t *destination,uint32_t target){
 uint8_t *p=net_tx_buffer();uint64_t mac=net_mac();memset(p,0,60);memcpy(p,destination,6);memcpy(p+6,&mac,6);put16(p+12,0x806);
 put16(p+14,1);put16(p+16,0x800);p[18]=6;p[19]=4;put16(p+20,op);memcpy(p+22,&mac,6);put32(p+28,net_config.local_ip);
 if(op==2)memcpy(p+32,destination,6);
 put32(p+38,target);return net_send(60);
}
uint8_t *udp_tx_payload(void){return net_tx_buffer()+42;}
bool udp_send(const uint8_t *body,size_t n){
 uint8_t *p=net_tx_buffer();uint64_t mac=net_mac();uint16_t length=(uint16_t)(n+8),check;
 if(n>1400)return false;
 if(!resolved){if(rdtsc()-last_arp>1000000){uint8_t broadcast[6]={255,255,255,255,255,255};if(!arp(1,broadcast,next_hop()))panic("ARP transmit");last_arp=rdtsc();}return false;}
 memset(p,0,42);memcpy(p,next_mac,6);memcpy(p+6,&mac,6);put16(p+12,0x800);p[14]=0x45;put16(p+16,(uint16_t)(28+n));put16(p+20,0x4000);p[22]=64;p[23]=17;
 put32(p+26,net_config.local_ip);put32(p+30,net_config.peer_ip);put16(p+24,finish(sum(p+14,20,0)));
 put16(p+34,net_config.local_port);put16(p+36,net_config.peer_port);put16(p+38,length);if(body!=p+42)memcpy(p+42,body,n);
 check=finish(sum(p+26,8,sum(p+34,length,17+length)));put16(p+40,check?check:65535);return net_send(42+n);
}
bool udp_receive(const uint8_t **body,size_t *n){
 const uint8_t *p;size_t length;uint32_t ip;uint16_t total,udp_length;
 if(!net_receive(&p,&length))return false;
 if(length>=42&&be16(p+12)==0x806&&be16(p+14)==1&&be16(p+16)==0x800&&p[18]==6&&p[19]==4){
  ip=be32(p+28);
  if(be16(p+20)==1&&be32(p+38)==net_config.local_ip){if(!arp(2,p+22,ip))panic("ARP response");}
  if(ip==next_hop()&&be32(p+38)==net_config.local_ip&&(be16(p+20)==1||be16(p+20)==2)){memcpy(next_mac,p+22,6);resolved=true;}
  net_release();return false;
 }
 if(length<42||be16(p+12)!=0x800||p[14]!=0x45||p[23]!=17||!p[22]||be16(p+20)&0x3fff||finish(sum(p+14,20,0))||be32(p+26)!=net_config.peer_ip||be32(p+30)!=net_config.local_ip){net_release();return false;}
 total=be16(p+16);udp_length=be16(p+38);
 if(total<28||total>length-14||udp_length<8||udp_length!=total-20||be16(p+34)!=net_config.peer_port||be16(p+36)!=net_config.local_port||
    (be16(p+40)&&finish(sum(p+26,8,sum(p+34,udp_length,17+udp_length))))){net_release();return false;}
 *body=p+42;*n=udp_length-8;return true;
}
void udp_release(void){net_release();}
