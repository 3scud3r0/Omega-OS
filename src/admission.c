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
