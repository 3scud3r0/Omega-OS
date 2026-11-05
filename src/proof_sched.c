#include "proof_sched.h"
#include "uart.h"
/* SipHash-2-4 over canonical fields; integrity tag, NOT a Lean proof or
 * adversarial isolation: the fixed demo key is public inside this kernel. */
static uint64_t rot(uint64_t x,unsigned n) { return (x<<n)|(x>>(64-n)); }
static void round_sip(uint64_t v[4]) {
 v[0]+=v[1]; v[1]=rot(v[1],13); v[1]^=v[0]; v[0]=rot(v[0],32);
 v[2]+=v[3]; v[3]=rot(v[3],16); v[3]^=v[2]; v[0]+=v[3];
 v[3]=rot(v[3],21); v[3]^=v[0]; v[2]+=v[1]; v[1]=rot(v[1],17); v[1]^=v[2]; v[2]=rot(v[2],32);
}
uint64_t omega_contract_token(const omega_task_t *t) {
 uint64_t v[4]={0x736f6d6570736575ULL^0x0706050403020100ULL,0x646f72616e646f6dULL^0x0f0e0d0c0b0a0908ULL,0x6c7967656e657261ULL^0x0706050403020100ULL,0x7465646279746573ULL^0x0f0e0d0c0b0a0908ULL};
 uint64_t words[11]; unsigned i;
 words[0]=t->id; words[1]=t->epoch;
 for(i=0;i<STATE_DIM;i++) { words[2+2*i]=(uint64_t)t->state[i].num; words[3+2*i]=(uint64_t)t->state[i].den; }
 words[10]=80ULL<<56;
 for(i=0;i<11;i++) { v[3]^=words[i]; round_sip(v); round_sip(v); v[0]^=words[i]; }
 v[2]^=255; for(i=0;i<4;i++) round_sip(v); return v[0]^v[1]^v[2]^v[3];
}
bool omega_verify_contract(const omega_task_t *t) {
 omega_rational_t norm={0,1},sq; omega_rational_t one={1,1}; unsigned i; int cmp;
 if(t->quarantined||!t->id||t->proof_hash!=omega_contract_token(t)) return false;
 for(i=0;i<STATE_DIM;i++) {
  if(!rational_mul(t->state[i],t->state[i],&sq)||!rational_add(norm,sq,&norm)) return false;
 }
 return rational_cmp(norm,one,&cmp)&&cmp<=0;
}
bool omega_checkpoint(omega_task_t *t,omega_task_t *checkpoint) {
 if(!omega_verify_contract(t)) return false;
 /* Both pointers refer to dedicated, page-sized physical frames. */
 memcpy(checkpoint,t,4096); return true;
}
bool omega_admit(omega_task_t *t,const omega_task_t *checkpoint) {
 if(omega_verify_contract(t)) return true;
 if(!omega_verify_contract(checkpoint)) panic("checkpoint integrity failure");
 memcpy(t,checkpoint,4096); t->quarantined=true;
 uart_puts("IMMUNE quarantine + full frame rollback id="); uart_put_dec(t->id); uart_puts("\n"); return false;
}
