#include "consensus.h"
#include "runtime.h"
#include "incubator.h"
#include "mmu.h"
#include "udp.h"
#include "net.h"
#include "tunnel.h"
#include "uart.h"
#include "crypto/monocypher.h"
#include "crypto/monocypher-ed25519.h"
#define LINEAGE_MAGIC 0x34454741454e494cULL
#define STATE_MAGIC 0x34544f564147454dULL
cluster_config_t cluster_config;
static tunnel_t channels[3];
static uint64_t *durable=(uint64_t *)0x40007000;
static lineage_record_t *published=(lineage_record_t *)LINEAGE_VA;
static lineage_record_t *proposal_checkpoint=(lineage_record_t *)0x40008000;
static lineage_record_t pending,assemblies[3];
static unsigned assembly_mask[3],transmit_chunk[3];
static uint8_t hello[3][104],plain[1200],app[1100];
static bool ready,announced;
static uint64_t authorized_epoch;static uint8_t authorized_hash[32];
static int compare(const void *a,const void *b,size_t n){const uint8_t *x=a,*y=b;uint8_t d=0;for(size_t i=0;i<n;i++)d|=x[i]^y[i];return d;}
static void statement(uint8_t out[120],const lineage_record_t *r){
 memcpy(out,"OMEGA-QUORUM-V4",16);memcpy(out+16,cluster_config.id,32);memcpy(out+48,&r->epoch,8);memcpy(out+56,r->previous,32);memcpy(out+88,r->hash,32);
}
static bool content(const lineage_record_t *r){
 uint8_t hash[32];if(r->magic!=LINEAGE_MAGIC||!r->epoch||(r->voters&~7ULL))return false;
 crypto_blake2b(hash,32,r->payload,sizeof r->payload);if(crypto_verify32(hash,r->hash))return false;
 if(compare(r->payload,"OMEGA-LINEAGE-V4",16))return false;
 for(unsigned i=0;i<16;i++){omega_organism_t o;memcpy(&o,r->payload+16+i*sizeof o,sizeof o);if(o.lineage!=i+1||o.generation>(1ULL<<62)||!o.cycles||!o.bytes||o.bytes>4096||o.value.num!=24||o.value.den!=1||o.genome[95])return false;}
 for(unsigned i=0;i<sizeof r->padding;i++)if(r->padding[i])return false;
 return true;
}
static bool signature(const lineage_record_t *r,unsigned id){uint8_t text[120];statement(text,r);return !crypto_ed25519_check(r->signatures[id],cluster_config.member[id].edkey,text,sizeof text);}
bool consensus_certificate(const lineage_record_t *r){unsigned votes=0;if(!content(r))return false;for(unsigned i=0;i<3;i++)if(r->voters&(1ULL<<i)){if(!signature(r,i))return false;votes++;}return votes>=2&&(r->voters&1);}
bool consensus_gate(uint64_t va,const void *data){if(va!=LINEAGE_VA)return true;const lineage_record_t *r=data;return cluster_config.enabled&&ready&&consensus_certificate(r)&&r->epoch==authorized_epoch&&!crypto_verify32(r->hash,authorized_hash);}
void consensus_provision(uint8_t *p,size_t n){
 uint8_t digest[32],pub[32],seed[32];uint32_t self,size;
 if(n!=400||compare(p,"OMCLUST4",8))panic("cluster boot module domain");
 memcpy(&size,p+8,4);memcpy(&self,p+12,4);if(size!=400||self>=3)panic("cluster module membership");
 crypto_blake2b(digest,32,p,368);if(crypto_verify32(digest,p+368))panic("cluster module checksum");
 memcpy(cluster_config.member,p+128,240);crypto_blake2b(digest,32,p+128,240);if(crypto_verify32(digest,p+96))panic("cluster identity hash");
 memcpy(cluster_config.id,digest,32);cluster_config.self=self;
 for(unsigned i=0;i<3;i++){cluster_member_t *m=&cluster_config.member[i];if(m->id!=i+1||!m->ip||!m->port)panic("cluster member address");for(unsigned j=0;j<i;j++)if(m->ip==cluster_config.member[j].ip||!crypto_verify32(m->edkey,cluster_config.member[j].edkey)||!crypto_verify32(m->xkey,cluster_config.member[j].xkey))panic("duplicate cluster identity");}
 memcpy(cluster_config.xsecret,p+32,32);memcpy(seed,p+64,32);crypto_x25519_public_key(pub,cluster_config.xsecret);if(crypto_verify32(pub,cluster_config.member[self].xkey))panic("cluster X25519 identity");
 crypto_ed25519_key_pair(cluster_config.edsecret,pub,seed);if(crypto_verify32(pub,cluster_config.member[self].edkey))panic("cluster Ed25519 identity");
 crypto_wipe(seed,32);crypto_wipe(p+32,64);
 memcpy(&net_config.mask,p+16,4);memcpy(&net_config.gateway,p+20,4);net_config.local_ip=cluster_config.member[self].ip;net_config.local_port=cluster_config.member[self].port;
 unsigned other=(self+1)%3;net_config.peer_ip=cluster_config.member[other].ip;net_config.peer_port=cluster_config.member[other].port;net_config.configured=true;cluster_config.enabled=true;
}
static void entropy(uint8_t out[48]){uint32_t a,b,c,d;__asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1),"c"(0));if(!(c&(1U<<30)))panic("cluster requires RDRAND");for(unsigned i=0;i<6;i++){uint64_t v;uint8_t ok=0;for(unsigned n=0;n<64&&!ok;n++)__asm__ volatile("rdrand %0;setc %1":"=r"(v),"=qm"(ok));if(!ok)panic("cluster entropy failure");memcpy(out+8*i,&v,8);}}
static void map(uint64_t va){uint64_t p=pmm_alloc_frame();if(!p||!mmu_map(boot_pml4,va,p,PAGE_WRITE))panic("cluster SLS frame");(void)sls_register(va);}
static void flush(void){if(!mmu_sls_commit(boot_pml4))panic("cluster durable transaction");}
static bool eligible(const lineage_record_t *r){return r->epoch==published->epoch+1&&(!published->epoch?!compare(r->previous,(uint8_t[32]){0},32):!crypto_verify32(r->previous,published->hash));}
static bool lock_vote(lineage_record_t *r){
 if(!content(r)||!signature(r,0)||!eligible(r))return false;
 if(durable[2]>r->epoch||(durable[2]==r->epoch&&crypto_verify32((uint8_t *)durable+32,r->hash)))return false;
 if(durable[2]!=r->epoch){durable[2]=r->epoch;memcpy((uint8_t *)durable+32,r->hash,32);flush();}
 uint8_t text[120];statement(text,r);crypto_ed25519_sign(r->signatures[cluster_config.self],cluster_config.edsecret,text,sizeof text);r->voters|=1ULL<<cluster_config.self;return true;
}
static void publish(const lineage_record_t *r){
 if(!consensus_certificate(r)||r->epoch<=published->epoch)return;
 /* A quorum certificate authenticates a complete application checkpoint.
  * A lagging node may install it without replaying missing local LBAs. */
 if(r->epoch==published->epoch+1&&!eligible(r))return;
 authorized_epoch=r->epoch;memcpy(authorized_hash,r->hash,32);
 memcpy(published,r,4096);flush();
 uart_puts("QUORUM durable lineage epoch=");uart_put_dec(r->epoch);uart_puts(" voters=");uart_put_hex(r->voters);uart_puts("\n");announced=true;
}
static bool send(unsigned id,const uint8_t *p,size_t n){udp_select(cluster_config.member[id].ip,cluster_config.member[id].port);uint8_t *wire=udp_tx_payload();size_t len=tunnel_seal(&channels[id],wire,p,n);return len&&udp_send(wire,len);}
static void fragment(unsigned id,const lineage_record_t *r,uint8_t type){unsigned chunk=transmit_chunk[id]%4;memset(app,0,16);app[0]=type;app[1]=(uint8_t)cluster_config.self;memcpy(app+8,&r->epoch,8);app[2]=(uint8_t)chunk;memcpy(app+16,(const uint8_t *)r+chunk*1024,1024);if(send(id,app,1040))transmit_chunk[id]++;}
static void receive(unsigned id,const uint8_t *p,size_t n){
 if(n<16||p[1]!=id)return;
 if(p[0]==2&&cluster_config.self==0&&n==120){uint64_t epoch;memcpy(&epoch,p+8,8);if(epoch!=pending.epoch||crypto_verify32(p+16,pending.hash))return;
  lineage_record_t vote=pending;memcpy(vote.signatures[id],p+48,64);if(!signature(&vote,id))return;
  memcpy(pending.signatures[id],p+48,64);pending.voters|=1ULL<<id;
  if(consensus_certificate(&pending)){publish(&pending);}
  return;
 }
 if((p[0]!=1&&p[0]!=3)||id!=0||n!=1040||p[2]>3)return;
 uint64_t epoch;memcpy(&epoch,p+8,8);lineage_record_t *r=&assemblies[id];
 if(!assembly_mask[id]||r->epoch!=epoch){memset(r,0,4096);r->epoch=epoch;assembly_mask[id]=0;}
 memcpy((uint8_t *)r+p[2]*1024,p+16,1024);assembly_mask[id]|=1U<<p[2];if(assembly_mask[id]!=15)return;
 assembly_mask[id]=0;
 if(p[0]==3){if(consensus_certificate(r)){publish(r);}return;}
 if(lock_vote(r)){memset(app,0,120);app[0]=2;app[1]=(uint8_t)cluster_config.self;memcpy(app+8,&r->epoch,8);memcpy(app+16,r->hash,32);memcpy(app+48,r->signatures[cluster_config.self],64);(void)send(0,app,120);uart_puts("QUORUM signed durable vote epoch=");uart_put_dec(r->epoch);uart_puts("\n");}
}
static void propose(void){
 const void *nomination=runtime_nomination();if(!nomination||published->epoch==~0ULL)panic("cluster nomination/epoch exhausted");
 if(proposal_checkpoint->epoch>published->epoch){
  if(!content(proposal_checkpoint)||!signature(proposal_checkpoint,0)||!eligible(proposal_checkpoint))panic("recovered proposal integrity");
  memcpy(&pending,proposal_checkpoint,4096);if(!lock_vote(&pending))panic("recovered proposal lock");uart_puts("QUORUM recovered durable uncommitted proposal\n");return;
 }
 memset(&pending,0,sizeof pending);pending.magic=LINEAGE_MAGIC;pending.epoch=published->epoch+1;if(published->epoch)memcpy(pending.previous,published->hash,32);
 memcpy(pending.payload,nomination,2816);crypto_blake2b(pending.hash,32,pending.payload,sizeof pending.payload);pending.voters=1;uint8_t text[120];statement(text,&pending);crypto_ed25519_sign(pending.signatures[0],cluster_config.edsecret,text,sizeof text);
 memcpy(proposal_checkpoint,&pending,4096);flush();
 if(!lock_vote(&pending))panic("leader durable vote lock");
}
void consensus_run(void){
 uint8_t random[48];uint64_t last_hello[3]={0,0,0},last_data[3]={0,0,0};
 if(!cluster_config.enabled)return;
 if(!net_init())panic("cluster VirtIO-Net missing");
 map((uint64_t)durable);map(LINEAGE_VA);if(cluster_config.self==0)map((uint64_t)proposal_checkpoint);ready=true;
 if(durable[0]&&durable[0]!=STATE_MAGIC)panic("cluster vote metadata format");
 if(durable[0]){if(crypto_verify32((uint8_t *)durable+64,cluster_config.id))panic("cluster pins changed");}
 else{durable[0]=STATE_MAGIC;memcpy((uint8_t *)durable+64,cluster_config.id,32);}
 if(published->epoch&&!consensus_certificate(published))panic("recovered lineage certificate");
 authorized_epoch=published->epoch;memcpy(authorized_hash,published->hash,32);
 if(durable[1]==~0ULL)panic("cluster boot counter exhausted");
 durable[1]++;flush();
 for(unsigned i=0;i<3;i++)if(i!=cluster_config.self){uint8_t *floor=(uint8_t *)durable+128+64*i;uint64_t boot;memcpy(&boot,floor,8);entropy(random);
  if(!tunnel_init(&channels[i],cluster_config.xsecret,cluster_config.member[i].xkey,random,durable[1],boot,floor+8,net_config.local_ip,cluster_config.member[i].ip))panic("cluster tunnel DH");
  tunnel_hello(&channels[i],hello[i]);}
 crypto_wipe(random,48);crypto_wipe(cluster_config.xsecret,32);
 if(cluster_config.self==0&&!published->epoch)propose();
 uart_puts("QUORUM service ready; fixed leader 1, signed majority 2/3; no general BFT\n");
 if(published->epoch){uart_puts("QUORUM recovered certified lineage epoch=");uart_put_dec(published->epoch);uart_puts("\n");}
 uint64_t start=rdtsc(),last_burst=rdtsc(),last_resumed_epoch=0;bool isolated=false;
 for(;;){
  for(unsigned i=0;i<3;i++)if(i!=cluster_config.self){udp_select(cluster_config.member[i].ip,cluster_config.member[i].port);
   if(rdtsc()-last_hello[i]>30000000ULL){(void)udp_send(hello[i],104);last_hello[i]=rdtsc();}
   if(cluster_config.self==0&&channels[i].established&&rdtsc()-last_data[i]>2000000ULL){if(pending.epoch>published->epoch&&(!published->epoch||((transmit_chunk[i]/4)&1)==0)){fragment(i,&pending,1);}else{fragment(i,published,3);}last_data[i]=rdtsc();}
  }
  const uint8_t *packet;size_t n;uint32_t ip;uint16_t port;
  if(udp_receive_from(&packet,&n,&ip,&port)){unsigned id;for(id=0;id<3;id++)if(id!=cluster_config.self&&ip==cluster_config.member[id].ip&&port==cluster_config.member[id].port)break;
   if(id<3){bool changed=false;if(n==104&&packet[8]==1){if(tunnel_accept(&channels[id],packet,n,&changed)&&changed){uint8_t *floor=(uint8_t *)durable+128+64*id;memcpy(floor,&channels[id].peer_boot,8);memcpy(floor+8,channels[id].saved_hello,32);flush();}}
    else{size_t length;if(tunnel_open(&channels[id],plain,&length,packet,n))receive(id,plain,length);}}
   udp_release();
  }
  if(!published->epoch&&!isolated&&rdtsc()-start>2000000000ULL){uart_puts("QUORUM isolated: lineage epoch remains 0; local metadata generations independent\n");isolated=true;}
  if(announced){uart_puts("TEST quorum certified lineage persisted; service remains available\n");announced=false;}
  /* Retain the same compiler process/arena across bursts. Network service
   * resumes between bounded bursts; no host compiler or child tool runs. */
  if(published->epoch>last_resumed_epoch&&rdtsc()-last_burst>10000000000ULL){last_resumed_epoch=published->epoch;runtime_resume();last_burst=rdtsc();if(cluster_config.self==0)propose();}
  cpu_pause();
 }
}
