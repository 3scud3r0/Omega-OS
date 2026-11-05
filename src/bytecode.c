#include "bytecode.h"
bool bytecode_run(const void *capsule,size_t size,omega_rational_t *out) {
 const uint8_t *p=capsule; uint64_t magic,n; omega_rational_t stack[32]; unsigned sp=0,gas=0; size_t pc=16;
 if(size<17||size>4096) return false;
 memcpy(&magic,p,8);memcpy(&n,p+8,8);if(magic!=BYTECODE_MAGIC||n!=size-16) return false;
 while(pc<size&&++gas<=256){uint8_t op=p[pc++];omega_rational_t a,b,q;
  if(op==0){if(pc!=size||sp!=1)return false;*out=stack[0];return true;}
  if(op==1){if(size-pc<16||sp==32)return false;memcpy(&q,p+pc,16);pc+=16;if(!rational_valid(q))return false;stack[sp++]=q;continue;}
  if(op==6){omega_rational_t zero={0,1};if(!sp||!rational_sub(zero,stack[sp-1],&q))return false;stack[sp-1]=q;continue;}
  if(sp<2)return false;
  b=stack[--sp];a=stack[sp-1];
  if(op==2){if(!rational_add(a,b,&q))return false;}
  else if(op==3){if(!rational_sub(a,b,&q))return false;}
  else if(op==4){if(!rational_mul(a,b,&q))return false;}
  else if(op==5){omega_rational_t inv;if(!rational_make(b.den,b.num,&inv)||!rational_mul(a,inv,&q))return false;}
  else return false;
  stack[sp-1]=q;
 }
 return false;
}
