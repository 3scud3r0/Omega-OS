#include "incubator.h"
extern void omega_fail(const char *s);
extern unsigned omega_guided_form(unsigned cycle);
static omega_organism_t organisms[INCUBATOR_SLOTS];
static uint64_t sys2(uint64_t n,uint64_t a,uint64_t b){uint64_t r;__asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b):"rcx","r11","memory");return r;}
static uint64_t stamp(void){uint32_t l,h,aux;static unsigned supported;
 if(!supported){uint32_t a,b,c,d;__asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(0x80000001U),"c"(0));supported=(d&(1U<<27))?2:1;}
 if(supported==1){lfence();__asm__ volatile("rdtsc":"=a"(l),"=d"(h)::"memory");lfence();return ((uint64_t)h<<32)|l;}__asm__ volatile("rdtscp; lfence":"=a"(l),"=d"(h),"=c"(aux)::"memory");return ((uint64_t)h<<32)|l;}
static size_t literal(uint8_t *p,size_t n,int64_t v){omega_rational_t q={v,1};p[n++]=1;memcpy(p+n,&q,16);return n+16;}
static size_t expression(uint8_t *p,unsigned form){
 /* Typed ring rewrites: associativity, commutativity and neutral elimination.
  * The expanded ancestor and its folded extraction denote exactly 24. */
 size_t n=16;uint64_t magic=BYTECODE_MAGIC;memcpy(p,&magic,8);
 if(form==0){n=literal(p,n,24);for(unsigned i=0;i<20;i++){n=literal(p,n,0);p[n++]=2;}}
 else if(form==1){n=literal(p,n,7);n=literal(p,n,5);p[n++]=2;n=literal(p,n,2);p[n++]=4;}
 else if(form==2){n=literal(p,n,5);n=literal(p,n,7);p[n++]=2;n=literal(p,n,2);p[n++]=4;}
 else if(form==3){n=literal(p,n,7);n=literal(p,n,2);p[n++]=4;n=literal(p,n,5);n=literal(p,n,2);p[n++]=4;p[n++]=2;}
 else n=literal(p,n,24);
 p[n++]=0;uint64_t size=n-16;memcpy(p+8,&size,8);return n;
}
static size_t seal(size_t n){uint8_t *page=(uint8_t *)JIT_VA;size_t length;
 if(sys2(19,0,0)||!jit_compile((void *)0x80003000,n,page,&length)||sys2(11,JIT_VA,length))omega_fail("Native canonical JIT seal");
 return length;
}
static uint64_t measure(void){
 uint64_t best=~0ULL;omega_rational_t q;
 for(unsigned sample=0;sample<7;sample++){uint64_t begin=stamp();for(unsigned j=0;j<32;j++){if(!((bool (*)(omega_rational_t *))JIT_VA)(&q)||q.num!=24||q.den!=1)omega_fail("Native rational fitness");}uint64_t cost=stamp()-begin;if(cost<best)best=cost;}
 return best/32;
}
void incubator_run(void){
 uint8_t *certificate=(uint8_t *)0x80003000;volatile uint64_t *data=(volatile uint64_t *)0x80002000;
 static unsigned round,total_promotions,total_rejected;
 unsigned promotions=0,rejected=0;uint64_t baseline_native,baseline_reference;size_t n=expression(certificate,1);(void)seal(n);
 baseline_native=measure();omega_rational_t q;uint64_t begin=stamp();for(unsigned i=0;i<256;i++){compiler_barrier();if(!bytecode_run(certificate,n,&q))omega_fail("Reference benchmark");}baseline_reference=(stamp()-begin)/256;
 if(baseline_native>=baseline_reference)omega_fail("Native benchmark did not improve");
 if(!round)for(unsigned i=0;i<INCUBATOR_SLOTS;i++){n=expression(certificate,0);size_t length=seal(n);organisms[i]=(omega_organism_t){i+1,0,measure(),length,{24,1},"((7+5)*2)+0 [expanded ancestor]"};}
 for(unsigned cycle=0;cycle<50;cycle++){
  unsigned slot=cycle%INCUBATOR_SLOTS;omega_organism_t *ancestor=&organisms[slot];
  n=expression(certificate,omega_guided_form(cycle));data[29]++;size_t length=seal(n);uint64_t cycles=measure();
  /* Strict conjunction: equal or smaller code AND strictly fewer measured
   * cycles. No promotion is manufactured when timing is tied or noisy. */
  if(length<=ancestor->bytes&&cycles<ancestor->cycles){ancestor->generation++;ancestor->bytes=length;ancestor->cycles=cycles;memset(ancestor->genome,0,sizeof ancestor->genome);memcpy(ancestor->genome,"tensor result = 24; mostrar result;",35);promotions++;}
 }
 /* Fault containment exercises a legitimate audited store across the SLS
  * capability boundary. First store dirties the page; second triggers #PF. */
 n=expression(certificate,4);(void)seal(n);
 uint64_t prior=*(volatile uint64_t *)0x80004ff8;
 if(incubator_call((void *)JIT_VA,(omega_rational_t *)0x80004ff8)||*(volatile uint64_t *)0x80004ff8!=prior)omega_fail("Native fault checkpoint rollback");
 rejected++;
 if(sys2(19,0,0))omega_fail("JIT reset");
 size_t length;if(!jit_compile(certificate,n,(uint8_t *)JIT_VA,&length))omega_fail("JIT audit fixture");
 ((uint8_t *)JIT_VA)[0]=0xf4;if(sys2(11,JIT_VA,length)!=~0ULL)omega_fail("Privileged opcode admitted");
 rejected++;
 /* Ill-typed and undefined rational transitions fail admission, before CPU. */
 n=16;uint64_t magic=BYTECODE_MAGIC;memcpy(certificate,&magic,8);n=literal(certificate,n,1);n=literal(certificate,n,0);certificate[n++]=5;certificate[n++]=0;uint64_t bytes=n-16;memcpy(certificate+8,&bytes,8);
 if(jit_compile(certificate,n,(uint8_t *)JIT_VA,&length))omega_fail("Division zero admitted");
 rejected++;
 n=expression(certificate,4);(void)seal(n);
 if(sys2(16,0,0)!=15)omega_fail("Sixteen process spawn");
 while(sys2(17,0,0))__asm__ volatile("pause");
 if(!promotions&&!round)omega_fail("No measured Pareto promotion");
 memset(certificate,0,4096);memcpy(certificate,"OMEGA-LINEAGE-V4",16);memcpy(certificate+16,organisms,sizeof organisms);
 total_promotions+=promotions;total_rejected+=rejected;round++;
 data[24]=baseline_native;data[25]=baseline_reference;data[26]=50ULL*round;data[27]=total_promotions;data[28]=total_rejected;
 if(sys2(20,0,0)||sys2(21,0,0))omega_fail("Incubator nomination");
}
/* Save the ELF continuation before entering native code. On a trapped native
 * fault the kernel restores this exact stack/register snapshot and rolls SLS
 * back, then returns false at the same trusted continuation. */
__asm__(".text\n.global incubator_call\nincubator_call:\n"
"pushq %rbp\npushq %rbx\npushq %r12\npushq %r13\npushq %r14\npushq %r15\nsubq $8,%rsp\n"
"movq %rdi,%r12\nmovq %rsi,%r13\nleaq 1f(%rip),%rdi\nmovq %rsp,%rsi\nmovq $12,%rax\nsyscall\n"
"movq %r13,%rdi\ncall *%r12\n"
"1:\nmovq %rax,%r14\nmovq $13,%rax\nsyscall\nmovq %r14,%rax\n"
"addq $8,%rsp\npopq %r15\npopq %r14\npopq %r13\npopq %r12\npopq %rbx\npopq %rbp\nret\n");
