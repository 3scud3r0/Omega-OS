#include "admission.h"
/* Total, deterministic stack checker/interpreter, no back edges, memory access
 * or arbitrary calls. ASSERT proves the evaluated integer equality, not Lean. */
bool vm_verify(const uint8_t *code,size_t length) {
 uint64_t stack[16]; uint8_t kind[16]; unsigned sp=0; size_t pc=0; bool assertion=false;
 if(!length||length>256) return false;
 while(pc<length) {
  uint8_t op=code[pc++]; uint64_t a,b;
  switch(op) {
   case 0: return assertion&&sp==0&&pc==length;
   case 1: if(pc==length||sp==16) return false; kind[sp]=0; stack[sp++]=code[pc++]; break;
   case 2: if(sp<2||kind[sp-1]||kind[sp-2]) return false; b=stack[--sp]; a=stack[sp-1]; if(__builtin_add_overflow(a,b,&stack[sp-1])) return false; break;
   case 3: if(sp<2||kind[sp-1]||kind[sp-2]) return false; b=stack[--sp]; stack[sp-1]=(stack[sp-1]==b); kind[sp-1]=1; break;
   case 4: if(!sp||kind[sp-1]!=1||stack[--sp]!=1) return false; assertion=true; break;
   default: return false;
  }
 }
 return false;
}
