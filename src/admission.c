#include "admission.h"
#include "crypto/monocypher-ed25519.h"
#include "uart.h"
#include "capsule.h"
const uint8_t *admission_code(size_t *length) {
 const uint8_t *m=capsule_message; uint32_t n,v; uint8_t bad[64],wrong_key[32];
 static const uint8_t domain[16]="OMEGA-CAPSULE-2";
 if(sizeof(capsule_message)<24) panic("short capsule");
 for(unsigned i=0;i<16;i++) if(m[i]!=domain[i]) panic("capsule domain");
 memcpy(&n,m+16,4); memcpy(&v,m+20,4);
 if(!n||n>4096||v>256||24ULL+n+v!=sizeof(capsule_message)) panic("capsule lengths");
 if(crypto_ed25519_check(capsule_signature,authority_public,m,sizeof(capsule_message))) panic("Ed25519 authority rejected");
 memcpy(wrong_key,authority_public,32); wrong_key[0]^=1;
 if(!crypto_ed25519_check(capsule_signature,wrong_key,m,sizeof(capsule_message))) panic("wrong authority accepted");
 memcpy(bad,capsule_signature,64); bad[0]^=1;
 if(!crypto_ed25519_check(bad,authority_public,m,sizeof(capsule_message))) panic("tampered signature accepted");
 if(!vm_verify(m+24+n,v)||vm_verify((const uint8_t *)"\x02\x00",2)) panic("VM proof admission");
 uart_puts("TEST Ed25519 external signature + tamper rejection + typed stack VM PASS\n");
 *length=n; return m+24;
}

const uint8_t *admission_compiler(size_t *length) {
 uint32_t n,v; static const uint8_t domain[16]="OMEGA-COMPILER3";
 if(sizeof compiler_message<24) panic("compiler capsule size");
 for(unsigned i=0;i<16;i++) if(compiler_message[i]!=domain[i]) panic("compiler capsule domain");
 memcpy(&n,compiler_message+16,4);memcpy(&v,compiler_message+20,4);
 if(n>1024*1024||24ULL+n+v!=sizeof compiler_message||!vm_verify(compiler_message+24+n,v)||
    crypto_ed25519_check(compiler_signature,authority_public,compiler_message,sizeof compiler_message)) panic("compiler authority admission");
 *length=n;return compiler_message+24;
}

const uint8_t *admission_cortex(size_t *length){
 uint32_t n,v;const uint8_t *m=cortex_message;static const uint8_t domain[16]="OMEGA-CORTEX-1";
 if(sizeof cortex_message<24)panic("cortex capsule length");
 for(unsigned i=0;i<16;i++)if(m[i]!=domain[i])panic("cortex capsule domain");
 memcpy(&n,m+16,4);memcpy(&v,m+20,4);
 if(n>1024*1024||24ULL+n+v!=sizeof cortex_message||!vm_verify(m+24+n,v)||crypto_ed25519_check(cortex_signature,authority_public,m,sizeof cortex_message))panic("cortex signature admission");
 *length=n;return m+24;
}
