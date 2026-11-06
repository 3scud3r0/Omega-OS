#include "omega_value.h"
#include "bytecode.h"
#include "incubator.h"
static uint8_t arena[8*1024*1024] ALIGN_4K;
static size_t used;
uint64_t ov_gas;
const char *ov_input;
V ov_output;
static uint64_t syscall(uint64_t n,uint64_t a){uint64_t r;__asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a):"rcx","r11","memory");return r;}
void *memcpy(void *d,const void *s,size_t n){uint8_t *a=d;const uint8_t *b=s;for(size_t i=0;i<n;i++)a[i]=b[i];return d;}
void *memset(void *p,int c,size_t n){uint8_t *b=p;for(size_t i=0;i<n;i++)b[i]=(uint8_t)c;return p;}
void *ov_alloc(size_t n){n=(n+7)&~(size_t)7;if(n>sizeof arena-used)omega_fail("Omega static arena exhausted");void *p=arena+used;used+=n;memset(p,0,n);return p;}
void omega_fail(const char *s){char *p=(char *)0x80003000;size_t n=0;while(s[n]&&n<96){p[n]=s[n];n++;}p[n]=0;(void)syscall(60,n);for(;;)__asm__ volatile("pause");}
static void emit(V nodes,int64_t id,uint8_t *out,size_t *n,unsigned depth){
 if(depth>24||id<0||(uint64_t)id>=nodes->n||*n>1000)omega_fail("Backend graph budget");
 V node=nodes->a[id],op=o_get(node,o_str("op")),args=o_get(node,o_str("args"));
 if(!ov_cmp(op->s,"num")){V value=o_number(o_get(node,o_str("v")));out[(*n)++]=1;memcpy(out+*n,&value->q,16);*n+=16;return;}
 if(!ov_cmp(op->s,"neg")){emit(nodes,o_long(args->a[0]),out,n,depth+1);out[(*n)++]=6;return;}
 if(args->n!=2)omega_fail("Scalar backend arity");
 emit(nodes,o_long(args->a[0]),out,n,depth+1);emit(nodes,o_long(args->a[1]),out,n,depth+1);
 if(op->n!=1)omega_fail("Scalar backend operation");
 switch(op->s[0]){case '+':out[(*n)++]=2;break;case '-':out[(*n)++]=3;break;case '*':out[(*n)++]=4;break;case '/':out[(*n)++]=5;break;default:omega_fail("Unsupported scalar backend");}
}
static V pair(const char *op,V a,V b){V x=o_list();o_push(x,o_str(op));o_push(x,a);if(b)o_push(x,b);return x;}
/* Finite PUCT port of omega_synth_mcts/search.py: exact tensor priors and
 * rewards, affine grammar a*x+b, explicit node/iteration budgets. */
static void tensor_mcts(int64_t *best_a,int64_t *best_b,unsigned *visited){
 struct Node {int parent,depth,a,b;unsigned visits;omega_rational_t reward,prior;} nodes[31];
 unsigned total=1;uint64_t seed=47;nodes[0]=(struct Node){-1,0,0,0,0,{0,1},{1,1}};
 omega_rational_t weights[2][3]={{{1,2},{1,3},{1,4}},{{1,3},{1,5},{1,6}}};
 for(unsigned iteration=0;iteration<192;iteration++){
  unsigned at=0,path[3]={0,0,0},length=1;
  while(nodes[at].depth<2){unsigned child=0;omega_rational_t best={-1,1};bool found=false;
   bool expanded=false;for(unsigned j=1;j<total;j++)if(nodes[j].parent==(int)at)expanded=true;
   if(!expanded){if(total+5>31)omega_fail("MCTS node budget");for(int k=-2;k<=2;k++){
    omega_rational_t features[3]={{k*k,1},{k<0?-k:k,1},{1,1}},prior={1,1},v;
    for(unsigned t=0;t<3;t++){if(!rational_mul(weights[nodes[at].depth][t],features[t],&v)||!rational_add(prior,v,&prior))omega_fail("Tensor prior arithmetic");}
    nodes[total++]=(struct Node){(int)at,nodes[at].depth+1,nodes[at].depth?nodes[at].a:k,k,0,{0,1},prior};
   }}
   for(unsigned j=1;j<total;j++)if(nodes[j].parent==(int)at){omega_rational_t mean={0,1},explore,score,factor;unsigned root=1;
    while((root+1)*(root+1)<=nodes[at].visits+1)root++;
    if(nodes[j].visits&&!rational_make(nodes[j].reward.num,nodes[j].reward.den*(int64_t)nodes[j].visits,&mean))omega_fail("MCTS mean");
    if(!rational_make(root,(int64_t)(nodes[j].visits+1),&factor)||!rational_mul(nodes[j].prior,factor,&explore)||!rational_add(mean,explore,&score))omega_fail("PUCT score");
    int cmp;if(!rational_cmp(score,best,&cmp))omega_fail("PUCT compare");
    seed=seed*6364136223846793005ULL+1;
    if(!found||cmp>0||(cmp==0&&(seed>>63))){best=score;child=j;found=true;}
   }
   if(!found)omega_fail("MCTS empty expansion");
   at=child;path[length++]=at;
  }
  int64_t loss=0;for(int64_t x=-2;x<=2;x++){int64_t delta=nodes[at].a*x+nodes[at].b-(2*x+1);loss+=delta<0?-delta:delta;}
  omega_rational_t reward={1,loss+1};
  for(unsigned j=0;j<length;j++){nodes[path[j]].visits++;if(!rational_add(nodes[path[j]].reward,reward,&nodes[path[j]].reward))omega_fail("MCTS reward");}
  if(!loss){*best_a=nodes[at].a;*best_b=nodes[at].b;*visited=iteration+1;return;}
 }
 omega_fail("MCTS finite budget exhausted");
}
unsigned omega_guided_form(unsigned cycle){
 /* All V objects from the preceding frontend/egraph call are ephemeral.
  * The durable pool contains copied scalar descriptors, never arena pointers. */
 used=0;ov_gas=0;int64_t a,b;unsigned iterations;tensor_mcts(&a,&b,&iterations);
 if(a!=2||b!=1||!iterations)omega_fail("Mutation PUCT contract");
 unsigned form=cycle<16?4:1+(cycle+(unsigned)(a+b))%4;
 V x=pair("const",o_int(7),0),y=pair("const",o_int(5),0),two=pair("const",o_int(2),0),expression;
 if(form==2)expression=pair("mul",pair("add",y,x),two);
 else if(form==3)expression=pair("add",pair("mul",x,two),pair("mul",y,two));
 else expression=pair("mul",pair("add",x,y),two);
 V request=o_obj();o_put(request,o_str("variables"),o_int(1));o_put(request,o_str("rounds"),o_int(4));o_put(request,o_str("max_nodes"),o_int(64));o_put(request,o_str("expression"),expression);
 V result=omega_egraph(request),value=o_get(result,o_str("value"));
 if(!o_truth(o_get(result,o_str("polynomial_equivalence_checked")))||value->n!=2||ov_cmp(value->a[0]->s,"const")||o_long(value->a[1])!=24)omega_fail("Typed mutation equivalence");
 return form;
}
void _start(void){
 volatile uint64_t *data=(volatile uint64_t *)0x80002000;uint8_t *capsule=(uint8_t *)0x80003000;
 while(!syscall(10,0))__asm__ volatile("pause");
 char source[512];size_t s=0;while(capsule[s]&&s<511){source[s]=(char)capsule[s];s++;}source[s]=0;
 V ir=omega_frontend(source),nodes=o_get(ir,o_str("nos")),symbols=o_get(ir,o_str("simbolos"));
 size_t n=16;uint64_t magic=BYTECODE_MAGIC;memcpy(capsule,&magic,8);emit(nodes,o_long(o_get(symbols,o_str("result"))),capsule,&n,0);capsule[n++]=0;
 {uint64_t bytes=n-16;memcpy(capsule+8,&bytes,8);}
 omega_rational_t expected;if(!bytecode_run(capsule,n,&expected))omega_fail("Compiled bytecode invalid");
 size_t native_length;if(!jit_compile(capsule,n,(uint8_t *)JIT_VA,&native_length))omega_fail("Native emitter");
 {uint64_t r;__asm__ volatile("syscall":"=a"(r):"a"(11ULL),"D"(JIT_VA),"S"(native_length):"rcx","r11","memory");if(r)omega_fail("Native W^X admission");}
 if(syscall(8,n)!=7)omega_fail("Delegated child admission");
 V request=o_obj(),expression=pair("mul",pair("add",pair("var",o_int(0),0),pair("const",o_int(0),0)),pair("const",o_int(1),0));
 o_put(request,o_str("variables"),o_int(1));o_put(request,o_str("rounds"),o_int(4));o_put(request,o_str("max_nodes"),o_int(64));o_put(request,o_str("expression"),expression);
 V result=omega_egraph(request),extracted=o_get(result,o_str("value"));
 if(!o_truth(o_get(result,o_str("polynomial_equivalence_checked")))||extracted->n!=2||ov_cmp(extracted->a[0]->s,"var")||o_long(extracted->a[1])!=0)omega_fail("Original Omega egraph result");
 omega_rational_t *persistent=(omega_rational_t *)0x80004100;
 if(!rational_make(1,3,persistent+1)||!rational_make(1,6,persistent+2)||!rational_add(persistent[1],persistent[2],persistent))omega_fail("Mapped SLS exact rational arithmetic");
 int64_t a,b;unsigned iterations;tensor_mcts(&a,&b,&iterations);if(a!=2||b!=1)omega_fail("Affine contract coefficients");
 data[8]=(uint64_t)o_long(o_get(result,o_str("nodes")));data[9]=(uint64_t)o_long(o_get(result,o_str("cost")));
 data[10]=(uint64_t)a;data[11]=(uint64_t)b;data[12]=iterations;memcpy((void *)(data+6),&expected,16);
 if(syscall(38,0))omega_fail("Cognitive SLS commit");
 while(!syscall(14,0))__asm__ volatile("pause");
 do {incubator_run();}while(syscall(23,0)==0);
 (void)syscall(3,0);omega_fail("Compiler returned after exit");
}
