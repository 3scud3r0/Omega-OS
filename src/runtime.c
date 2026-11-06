#include "runtime.h"
#include "mmu.h"
#include "ipc.h"
#include "uart.h"
#include "admission.h"
#include "smp.h"
#include "net.h"
#include "udp.h"
#include "bytecode.h"
#include "jit.h"
#include "consensus.h"
#include "crypto/monocypher.h"
#define TASKS 16U
#define USER_CODE 0x80000000ULL
#define USER_STACK 0x80001000ULL
#define USER_DATA 0x80002000ULL
static struct {
 uint64_t root,data,tx,ticks,calls,fault,address,error,pc_low,pc_high,sp_low,sp_high; interrupt_frame_t saved,guard; omega_rational_t expected; uint64_t jit,jit_length,certificate_size; bool dead,allocated,guarded;
} process[TASKS] ALIGN_64;
static omega_ring_t messages ALIGN_64;
static unsigned current,completed,preemptions;
static bool running,mutation_ready,child_started,compiler_reported,child_reported,baseline_ok,incubator_ok;
static unsigned batch_remaining,batch_finished,peak;
static bool parked;
static uint8_t nomination[4096] ALIGN_4K;
const void *runtime_nomination(void){return incubator_ok?nomination:(void *)0;}
static void baseline(void);
static void native_context(unsigned id,bool original);
static void destroy(unsigned id);
static const uint8_t *compiler_image;
static size_t compiler_size;
static omega_rational_t derived_result;
static uint64_t cognitive_phys;
static uint8_t *cognitive=(uint8_t *)0x40005000;
static char requested_source[512];
static size_t requested_size;
void runtime_source(const char *source,size_t n){if(n>=sizeof requested_source)panic("mutation source budget");memcpy(requested_source,source,n);requested_source[n]=0;requested_size=n?n+1:2;}
static void spawn_child(const void *capsule,size_t n);
static uint64_t syscall_kernel_stack[2048] ALIGN_4K;
uint64_t syscall_stack_top __attribute__((used));
uint64_t syscall_user_sp __attribute__((used));
uint64_t runtime_kernel_sp __attribute__((used));
extern void runtime_enter(interrupt_frame_t *frame);
extern void syscall_entry(void);
static void wrmsr(uint32_t reg,uint64_t value) { __asm__ volatile("wrmsr"::"c"(reg),"a"((uint32_t)value),"d"((uint32_t)(value>>32))); }
static void switch_root(uint64_t root) { __asm__ volatile("movq %0,%%cr3"::"r"(root):"memory"); }
/* Isolated top-level root + PDPT: supervisor kernel mappings shared, process
 * user branch at PDPT[2] private; user leaf permissions never inherited. */
static uint64_t create_root(void) {
 uint64_t root=pmm_alloc_frame(),pdpt=pmm_alloc_frame(); uint64_t *r=(uint64_t *)root;
 if(!root||!pdpt) panic("process page tables exhausted");
 memcpy(r,boot_pml4,4096); memcpy((void *)pdpt,(void *)(boot_pml4[0]&~4095ULL),4096);
 ((uint64_t *)pdpt)[1]&=~PAGE_USER; ((uint64_t *)pdpt)[2]=0; r[0]=pdpt|7; return root;
}
bool runtime_active(void) { return running; }
static bool choose(interrupt_frame_t *f) {
 unsigned i;
 for(i=1;i<=TASKS;i++) {
  unsigned next=(current+i)%TASKS;
  if(process[next].allocated&&!process[next].dead) {
   current=next; memcpy(f,&process[current].saved,sizeof *f);
   switch_root(process[current].root); return true;
  }
 }
 running=false; smp_timer_stop(); switch_root((uint64_t)boot_pml4); return false;
}
uint64_t runtime_trap(interrupt_frame_t *f) {
 if(f->vector==48) {
  smp_timer_eoi();
  if((f->cs&3)!=3) return 0;
  process[current].ticks++; preemptions++;
  memcpy(&process[current].saved,f,sizeof *f);
  if(process[current].ticks>=(current>=5?3000U:8U)) { process[current].dead=true; completed++; }
 } else {
  if((f->cs&3)!=3) panic("kernel exception during runtime");
  if(process[current].guarded&&f->rip>=JIT_VA&&f->rip<JIT_VA+process[current].jit_length&&(f->vector==0||f->vector==6||f->vector==13||f->vector==14)){
   uint64_t vector=f->vector; (void)sls_restore((uint64_t)cognitive);
   /* A restored checkpoint is already durable. Acknowledge both aliases,
    * so rollback itself does not masquerade as a subsequent mutation. */
   *mmu_leaf(boot_pml4,(uint64_t)cognitive)&=~PAGE_DIRTY;invlpg((uint64_t)cognitive);
   *mmu_leaf((uint64_t *)process[current].root,0x80004000)&=~PAGE_DIRTY;invlpg(0x80004000);
   memcpy(f,&process[current].guard,sizeof *f);f->rax=0;process[current].guarded=false;
   uart_puts("JIT fault rollback parent retained vector=");uart_put_dec(vector);uart_puts("\n");return 0;
  }
  if(baseline_ok&&current==0&&batch_remaining){if(f->vector!=14||process[current].dead||(f->error&7)!=7)panic("JIT RX write fault evidence");uint64_t address;__asm__ volatile("movq %%cr2,%0":"=r"(address));if(address!=JIT_VA)panic("JIT fault address");batch_remaining--;batch_finished++;uart_puts("TEST JIT RX write trapped #PF; child quarantined PASS\n");}
  process[current].fault=f->vector; process[current].error=f->error;
  __asm__ volatile("movq %%cr2,%0":"=r"(process[current].address));
  uart_puts("PROCESS quarantine id="); uart_put_dec(current+1); uart_puts(" vector="); uart_put_dec(f->vector); uart_puts("\n");
  process[current].dead=true; completed++;
 }
 return choose(f)?0:2;
}
uint64_t syscall_dispatch(interrupt_frame_t *f) {
 uint64_t result=~0ULL,value;
 if(!running||(f->cs&3)!=3) panic("syscall outside user runtime");
 if(f->rip<process[current].pc_low||f->rip>=process[current].pc_high||f->rsp<process[current].sp_low||f->rsp>process[current].sp_high) {
  process[current].fault=128; process[current].address=f->rsp;
  process[current].dead=true; completed++;
  return choose(f)?0:2;
 }
 process[current].calls++;
 switch(f->rax) {
  case 0: result=current+1; break;
  case 1: if(current==0&&ring_push(&messages,f->rdi)) result=0; break;
  case 2: if(current==1&&ring_pop(&messages,&value)) result=value; break;
  case 3:
   if(f->rdi) { uart_puts("PROCESS nonzero exit quarantined\n"); }
   process[current].dead=true; completed++;
   return choose(f)?0:2;
  case 4: if(!net_config.configured&&f->rdi>=14&&f->rdi<=1514&&net_send_page((void *)process[current].tx,(size_t)f->rdi)) result=0; break;
  case 8:
   if(current==5&&!child_started&&f->rdi<=4096&&bytecode_run((void *)process[current].tx,(size_t)f->rdi,&derived_result)) {
    if(process[current].jit_length){spawn_child((void *)process[current].tx,(size_t)f->rdi); result=7;}
   }
   break;
  case 9:
   if(current==0&&f->rdi>1&&f->rdi<=512&&!mutation_ready) {
    memcpy((void *)process[5].tx,(void *)process[0].tx,(size_t)f->rdi);
    ((char *)process[5].tx)[f->rdi-1]=0;mutation_ready=true;result=0;
   }
   break;
  case 10: result=mutation_ready;break;
  case 11: /* Seal only the caller's single, private JIT capability. */
   if(current==5&&f->rdi==JIT_VA&&f->rsi>0&&f->rsi<=4096){
    uint64_t *leaf=mmu_leaf((uint64_t *)process[current].root,JIT_VA);
    uint64_t count;memcpy(&count,(void *)(process[current].tx+8),8);
    if(leaf&&(*leaf&(PAGE_USER|PAGE_WRITE|PAGE_NX))==(PAGE_USER|PAGE_WRITE|PAGE_NX)&&count<=4080&&
       jit_audit((void *)process[current].tx,(size_t)count+16,(void *)process[current].jit,(size_t)f->rsi)){
     *leaf&=~PAGE_WRITE;invlpg(JIT_VA);
     if(!mmu_identity(process[current].jit,false))panic("JIT identity alias");
     *leaf&=~PAGE_NX;invlpg(JIT_VA);
     if((*leaf&(PAGE_WRITE|PAGE_NX))||(*mmu_leaf(boot_pml4,process[current].jit)&PAGE_WRITE)||!(*mmu_leaf(boot_pml4,process[current].jit)&PAGE_NX))panic("JIT W^X PTE invariant");
     process[current].jit_length=f->rsi;process[current].certificate_size=count+16;result=0;
    }
   }break;
  case 12: /* Trusted ELF recovery point and stack, never a JIT-controlled PC. */
   if(current==5&&process[current].jit_length&&f->rdi>=process[current].pc_low&&f->rdi<process[current].pc_high&&f->rsi>=process[current].sp_low&&f->rsi<=process[current].sp_high&&!process[current].guarded){
    process[current].guard=*f;process[current].guard.rflags=(f->rflags&0x8d5ULL)|0x202;process[current].guard.rip=f->rdi;process[current].guard.rsp=f->rsi;process[current].guarded=true;result=0;
   }break;
  case 13: process[current].guarded=false;result=0;break;
  case 14:
   if(current==5){if(child_reported&&compiler_reported&&process[0].dead&&process[1].dead&&process[2].dead&&process[3].dead&&process[4].dead){baseline();result=1;}else result=0;}break;
  case 16:
   if(current==5&&baseline_ok&&!batch_remaining&&process[5].jit_length){
    batch_finished=0;
    for(unsigned id=0;id<TASKS;id++)if(id!=5){if(process[id].allocated)destroy(id);native_context(id,false);batch_remaining++;}
    peak=TASKS;result=batch_remaining;uart_puts("RUNTIME 16 private Ring3 address spaces runnable simultaneously\n");
   }break;
  case 17:
   if(current==5){if(batch_remaining){f->rax=1;process[current].saved=*f;return choose(f)?0:2;}result=0;}break;
  case 19:
   if(current==5&&!process[current].guarded){uint64_t *leaf=mmu_leaf((uint64_t *)process[current].root,JIT_VA);if(leaf){*leaf|=PAGE_NX;invlpg(JIT_VA);if(!mmu_identity(process[current].jit,true))panic("JIT rearm identity");*leaf|=PAGE_WRITE;process[current].jit_length=0;result=0;}}break;
  case 20:
   if(current==5&&baseline_ok&&batch_finished==15){memcpy(nomination,(void *)process[current].tx,4096);incubator_ok=true;result=0;}break;
  case 21:
   if(current==5){uint64_t *d=(uint64_t *)process[current].data;
    uart_puts("JIT native TSC=");uart_put_dec(d[24]);uart_puts(" reference interpreter TSC=");uart_put_dec(d[25]);
    uart_puts("\nINCUBATOR cycles=");uart_put_dec(d[26]);uart_puts(" promotions=");uart_put_dec(d[27]);uart_puts(" rejected=");uart_put_dec(d[28]);uart_puts(" egraph/PUCT mutations=");uart_put_dec(d[29]);uart_puts("\n");result=0;
   }break;
  case 23:
   if(current==5&&incubator_ok&&cluster_config.enabled){process[current].saved=*f;process[current].saved.rax=0;parked=true;running=false;smp_timer_stop();switch_root((uint64_t)boot_pml4);return 2;}result=1;break;
  case 39:
   if(current!=5&&batch_remaining){omega_rational_t q;memcpy(&q,(void *)(process[current].data+48),16);if(q.num!=process[current].expected.num||q.den!=process[current].expected.den)panic("spawn native result");batch_remaining--;batch_finished++;process[current].dead=true;completed++;return choose(f)?0:2;}break;
  case 37:
   if(current==6) {
    omega_rational_t got;memcpy(&got,(void *)(process[6].data+48),16);
    if(got.num!=derived_result.num||got.den!=derived_result.den)panic("derived process rational result");
    child_reported=true;
    if(compiler_reported){memcpy(cognitive+72,&got,16);if(!mmu_sls_commit(boot_pml4))panic("child SLS commit");}
    process[current].dead=true;completed++;return choose(f)?0:2;
   }
   break;
  case 38:
   if(current==5&&child_started) {
    uint64_t *d=(uint64_t *)process[5].data;
    if(d[8]<3||d[9]!=1||d[10]!=2||d[11]!=1||!d[12])panic("cognitive motor evidence");
    omega_rational_t exact;memcpy(&exact,cognitive+256,16);if(exact.num!=1||exact.den!=2)panic("mapped SLS rational result");
    memset(cognitive,0,4096);memcpy(cognitive+256,&exact,16);memcpy(cognitive,"OMEGA-COG-V3",12);memcpy(cognitive+16,&derived_result,16);
    memcpy(cognitive+32,d+8,40);{uint64_t identity=net_mac();memcpy(cognitive+128,&identity,8);}compiler_reported=true;
    if(child_reported){memcpy(cognitive+72,&derived_result,16);if(!mmu_sls_commit(boot_pml4))panic("cognitive SLS commit");}
    result=0;
   }
   break;
  case 60:
   uart_puts("OMEGA USER ERROR: ");
   { const char *text=(const char *)process[current].tx; for(unsigned i=0;i<96&&text[i];i++)uart_putc(text[i]);uart_puts("\n"); }
   process[current].fault=60;process[current].dead=true;completed++;return choose(f)?0:2;
  case 7: result=net_config.configured?0:net_mac(); break;
  default: break;
 }
 f->rax=result;
 /* RIP/RFLAGS are never used blindly by SYSRET.
  * Validated low-canonical RIP/RSP permit the SYSRET fast return. */
 f->rflags=(f->rflags&0x8d5ULL)|0x202; return 3;
}

static uint64_t load_elf(unsigned id) {
 const uint8_t *elf=compiler_image;uint64_t phoff,entry;uint16_t phnum,phsize;
 if(compiler_size<64||elf[0]!=127||elf[1]!='E'||elf[2]!='L'||elf[3]!='F'||elf[4]!=2)panic("compiler ELF header");
 memcpy(&entry,elf+24,8);memcpy(&phoff,elf+32,8);memcpy(&phsize,elf+54,2);memcpy(&phnum,elf+56,2);
 if(phsize!=56||phnum>8||phoff>compiler_size||(uint64_t)phnum*56>compiler_size-phoff)panic("compiler ELF table");
 for(unsigned i=0;i<phnum;i++) {
  const uint8_t *h=elf+phoff+56*i;uint32_t type,flags;uint64_t off,va,filesz,memsz;
  memcpy(&type,h,4);memcpy(&flags,h+4,4);if(type!=1)continue;
  memcpy(&off,h+8,8);memcpy(&va,h+16,8);memcpy(&filesz,h+32,8);memcpy(&memsz,h+40,8);
  if(va<0x81000000||memsz>12*1024*1024||va+memsz>0x83000000||filesz>memsz||off>compiler_size||filesz>compiler_size-off||((flags&3)==3)||(va&4095))panic("compiler ELF segment");
  if(flags&1){process[id].pc_low=va;process[id].pc_high=va+filesz;}
  for(uint64_t n=0;n<memsz;n+=4096) {
   uint64_t p=pmm_alloc_frame();if(!p)panic("compiler physical frame budget");
   if(n<filesz){uint64_t copy=filesz-n;if(copy>4096)copy=4096;memcpy((void *)p,elf+off+n,(size_t)copy);}
   if(!mmu_map((uint64_t *)process[id].root,va+n,p,PAGE_USER|((flags&2)?PAGE_WRITE:0)|((flags&1)?0:PAGE_NX)))panic("ELF user map");
  }
 }
 if(entry<process[id].pc_low||entry>=process[id].pc_high)panic("compiler ELF entry");
 return entry;
}
static void compiler_context(unsigned id,uint64_t role) {
 interrupt_frame_t *f=&process[id].saved;
 process[id].allocated=true;process[id].root=create_root();process[id].data=pmm_alloc_frame();process[id].tx=pmm_alloc_frame();
 if(!process[id].data||!process[id].tx)panic("compiler metadata frames");
 *(uint64_t *)process[id].data=role;
 if(!mmu_map((uint64_t *)process[id].root,USER_DATA,process[id].data,PAGE_USER|PAGE_WRITE|PAGE_NX)||
    !mmu_map((uint64_t *)process[id].root,USER_DATA+4096,process[id].tx,PAGE_USER|(id==6?0:PAGE_WRITE)|PAGE_NX))panic("compiler IO pages");
 for(uint64_t va=0x80f00000;va<0x80f10000;va+=4096){uint64_t p=pmm_alloc_frame();if(!p||!mmu_map((uint64_t *)process[id].root,va,p,PAGE_USER|PAGE_WRITE|PAGE_NX))panic("compiler stack");}
 if(id==5&&!mmu_map((uint64_t *)process[id].root,0x80004000,cognitive_phys,PAGE_USER|PAGE_WRITE|PAGE_NX))panic("explicit SLS page capability");
 process[id].sp_low=0x80f00000;process[id].sp_high=0x80f10000;
 if(id==5){process[id].jit=pmm_alloc_frame();if(!process[id].jit||!mmu_map((uint64_t *)process[id].root,JIT_VA,process[id].jit,PAGE_USER|PAGE_WRITE|PAGE_NX))panic("private JIT allocation");}
 f->rip=load_elf(id);f->rsp=process[id].sp_high;f->cs=USER_CS;f->ss=USER_DS;f->rflags=0x202;
}
static void native_context(unsigned id,bool original){
 const uint8_t *code;size_t n;uint64_t text,stack;interrupt_frame_t *f;
 memset(&process[id],0,sizeof process[id]);process[id].allocated=true;process[id].root=create_root();
 process[id].data=pmm_alloc_frame();process[id].tx=pmm_alloc_frame();process[id].jit=pmm_alloc_frame();text=pmm_alloc_frame();stack=pmm_alloc_frame();
 if(!process[id].data||!process[id].tx||!process[id].jit||!text||!stack)panic("native child frame budget");
 code=admission_code(&n);memcpy((void *)text,code,n);memcpy((void *)process[id].jit,(void *)process[5].jit,4096);
 process[id].jit_length=process[5].jit_length;
 if(!bytecode_run((void *)process[5].tx,process[5].certificate_size,&process[id].expected))panic("native child certificate");
 uint64_t *d=(uint64_t *)process[id].data;d[0]=id;d[22]=1;d[23]=original?37:39;d[24]=(!original&&id==0)?1:0;
 if(!mmu_map((uint64_t *)process[id].root,USER_CODE,text,PAGE_USER)||!mmu_map((uint64_t *)process[id].root,USER_STACK,stack,PAGE_USER|PAGE_WRITE|PAGE_NX)||
 !mmu_map((uint64_t *)process[id].root,USER_DATA,process[id].data,PAGE_USER|PAGE_WRITE|PAGE_NX)||!mmu_map((uint64_t *)process[id].root,USER_DATA+4096,process[id].tx,PAGE_USER|PAGE_WRITE|PAGE_NX)||!mmu_map((uint64_t *)process[id].root,JIT_VA,process[id].jit,PAGE_USER))panic("native spawn mappings");
 process[id].pc_low=USER_CODE;process[id].pc_high=USER_CODE+4096;process[id].sp_low=USER_STACK;process[id].sp_high=USER_DATA;
 f=&process[id].saved;f->rip=USER_CODE;f->rsp=USER_DATA;f->cs=USER_CS;f->ss=USER_DS;f->rflags=0x202;
}
static void destroy(unsigned id){
 /* Only dead small agent spaces are reclaimed. The compiler's arena is never
  * inherited. Clear all user aliases before releasing executable frames. */
 uint64_t frames[32];unsigned count=0;uint64_t *root=(uint64_t *)process[id].root,*pdpt=(uint64_t *)(root[0]&0x000ffffffffff000ULL);
 if(!process[id].dead||id==5)panic("live process reclaim");
 uint64_t *pd=(uint64_t *)(pdpt[2]&0x000ffffffffff000ULL);
 for(unsigned a=0;a<512;a++)if(pd[a]&1){uint64_t *pt=(uint64_t *)(pd[a]&0x000ffffffffff000ULL);
  for(unsigned b=0;b<512;b++)if(pt[b]&1){uint64_t p=pt[b]&0x000ffffffffff000ULL;bool seen=false;pt[b]=0;for(unsigned j=0;j<count;j++)if(frames[j]==p)seen=true;if(!seen){if(count==32)panic("child memory quota");frames[count++]=p;}}
  if(!pmm_free_frame((uint64_t)pt))panic("child PT release");
 }
 if(!pmm_free_frame((uint64_t)pd)||!pmm_free_frame((uint64_t)pdpt)||!pmm_free_frame((uint64_t)root))panic("child tables release");
 for(unsigned j=0;j<count;j++)if(!pmm_free_frame(frames[j]))panic("child page release");
 memset(&process[id],0,sizeof process[id]);process[id].dead=true;
}
static void spawn_child(const void *capsule,size_t n){
 (void)capsule;(void)n;native_context(6,true);child_started=true;
 uart_puts("ADMISSION delegated rational bytecode capsule -> fresh Ring3 PID 7; native executable\n");
}
static void baseline(void){
 if(baseline_ok)return;
 if(preemptions<8||!*(uint64_t *)(process[0].data+8)||*(uint64_t *)(process[1].data+8)!=256||process[1].calls<257)panic("runtime completion");
 if(process[4].fault!=128||process[4].address!=0xffff800000000000ULL||process[0].ticks!=8||process[2].fault!=14||process[2].address!=0x90000000||(process[2].error&7)!=4||
    process[3].fault!=14||process[3].address!=USER_STACK||(process[3].error&0x15)!=0x15)panic("process isolation/NX fault evidence");
 uart_puts("TEST noncanonical syscall stack quarantined without SYSRET PASS\n");
 uart_puts("TEST private peer page PF + non-executable stack PF PASS\n");
 uart_puts("TEST preemption infinite agent revoked; private CR3; Ring3 IPC syscalls PASS\n");
 uart_puts("SYSCALL/SYSRET mean TSC ticks=");uart_put_dec(*(uint64_t *)(process[1].data+16));uart_puts("\n");baseline_ok=true;
}
bool runtime_run(void) {
 const uint8_t *code; size_t size; unsigned i; uint64_t start=rdtsc();
 for(i=0;i<TASKS;i++)process[i].dead=true;
 code=admission_code(&size); ring_init(&messages);
 { uint32_t lo,hi; __asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(0xc0000080)); wrmsr(0xc0000080,((uint64_t)hi<<32)|lo|0x800); }
 for(i=0;i<5;i++) {
  uint64_t text=pmm_alloc_frame(),stack=pmm_alloc_frame(); interrupt_frame_t *f=&process[i].saved;
  process[i].pc_low=USER_CODE;process[i].pc_high=USER_CODE+4096;process[i].sp_low=USER_STACK;process[i].sp_high=USER_DATA;
  process[i].allocated=true;process[i].dead=false;process[i].root=create_root(); process[i].data=pmm_alloc_frame(); process[i].tx=pmm_alloc_frame();
  if(!text||!stack||!process[i].data||!process[i].tx) panic("process memory exhausted");
  memcpy((void *)text,code,size); *(uint64_t *)process[i].data=i;
  if(!mmu_map((uint64_t *)process[i].root,USER_CODE,text,PAGE_USER)||
     !mmu_map((uint64_t *)process[i].root,USER_STACK,stack,PAGE_USER|PAGE_WRITE|PAGE_NX)||
     !mmu_map((uint64_t *)process[i].root,USER_DATA,process[i].data,PAGE_USER|PAGE_WRITE|PAGE_NX)||
     !mmu_map((uint64_t *)process[i].root,USER_DATA+4096,process[i].tx,PAGE_USER|PAGE_WRITE|PAGE_NX)) panic("private user mapping");
  if(i==0&&requested_size){memcpy((void *)process[0].tx,requested_source,requested_size);((uint64_t *)process[0].data)[21]=requested_size;}
  /* Only process 0 can read this marker; process 2's access must fault. */
  if(i==0&&!mmu_map((uint64_t *)process[i].root,0x90000000,process[i].data,PAGE_USER|PAGE_NX)) panic("private marker");
  f->rip=USER_CODE; f->rsp=USER_DATA; f->cs=USER_CS; f->ss=USER_DS; f->rflags=0x202;
 }
 {uint64_t p=pmm_alloc_frame();if(!p||!mmu_map(boot_pml4,(uint64_t)cognitive,p,PAGE_WRITE))panic("cognitive SLS frame");cognitive_phys=p;(void)sls_register((uint64_t)cognitive);}
 compiler_image=admission_compiler(&compiler_size);compiler_context(5,5);process[5].dead=false;
 syscall_stack_top=(uint64_t)&syscall_kernel_stack[2048];
 wrmsr(0xc0000081,((uint64_t)0x13<<48)|((uint64_t)8<<32));
 wrmsr(0xc0000082,(uint64_t)syscall_entry); wrmsr(0xc0000084,0x47700);
 { uint32_t lo,hi; __asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(0xc0000080)); wrmsr(0xc0000080,((uint64_t)hi<<32)|lo|1); }
 running=true; current=0; switch_root(process[0].root); smp_timer_start();
 runtime_enter(&process[0].saved);
 if(!compiler_reported||!child_reported||!child_started){
  (void)sls_restore((uint64_t)cognitive);
  uart_puts("ADMISSION rejected mutation; kernel retained prior SLS checkpoint\n");return false;
 }
 uart_puts("TEST original Omega frontend + egraph + tensor PUCT in Ring3 + dynamic child + exact SLS PASS\n");
 if(!baseline_ok||!incubator_ok||batch_remaining||batch_finished!=15||peak!=16)panic("V4 incubation/spawn completion");
 uart_puts("TEST native JIT W^X + 16 isolated processes + incubation PASS\n");
 uart_puts("SYSCALL runtime total TSC ticks="); uart_put_dec(rdtsc()-start); uart_puts("\n");return true;
}
void runtime_resume(void){
 if(!parked||!process[5].allocated||process[5].dead)panic("incubator continuation");
 parked=false;current=5;process[5].ticks=0;running=true;switch_root(process[5].root);smp_timer_start();runtime_enter(&process[5].saved);
 if(!parked||!incubator_ok||batch_remaining)panic("incubator resume completion");
 uart_puts("INCUBATOR persistent Ring3 process completed another 50-cycle burst\n");
}
/* A single BSP kernel stack is used only while IF is masked in syscalls.
 * User stacks are never dereferenced on entry. No SWAPGS state is needed. */
__asm__(
".text\n.global syscall_entry\nsyscall_entry:\n"
"movq %rsp,syscall_user_sp\nmovq syscall_stack_top,%rsp\n"
"pushq $0x1b\npushq syscall_user_sp\npushq %r11\npushq $0x23\npushq %rcx\npushq $0\npushq $128\n"
"pushq %rax\npushq %rbx\npushq %rcx\npushq %rdx\npushq %rbp\npushq %rsi\npushq %rdi\npushq %r8\npushq %r9\npushq %r10\npushq %r11\npushq %r12\npushq %r13\npushq %r14\npushq %r15\n"
"cld\nmovq %rsp,%rdi\nmovq %rsp,%rbx\nandq $-16,%rsp\ncall syscall_dispatch\nmovq %rbx,%rsp\ncmpq $2,%rax\nje runtime_return\ncmpq $3,%rax\nje syscall_restore\njmp runtime_restore\n"
"syscall_restore:\n"
"popq %r15\npopq %r14\npopq %r13\npopq %r12\npopq %r11\npopq %r10\npopq %r9\npopq %r8\npopq %rdi\npopq %rsi\npopq %rbp\npopq %rdx\npopq %rcx\npopq %rbx\npopq %rax\naddq $16,%rsp\n"
"movq (%rsp),%rcx\nmovq 16(%rsp),%r11\nmovq 24(%rsp),%rsp\nsysretq\n"
".global runtime_enter\nruntime_enter:\n"
"pushq %rbp\npushq %rbx\npushq %r12\npushq %r13\npushq %r14\npushq %r15\nmovq %rsp,runtime_kernel_sp\n"
"movq %rdi,%rsp\n"
".global runtime_restore\nruntime_restore:\n"
"popq %r15\npopq %r14\npopq %r13\npopq %r12\npopq %r11\npopq %r10\npopq %r9\npopq %r8\npopq %rdi\npopq %rsi\npopq %rbp\npopq %rdx\npopq %rcx\npopq %rbx\npopq %rax\naddq $16,%rsp\niretq\n"
".global runtime_return\nruntime_return:\ncli\nmovq runtime_kernel_sp,%rsp\npopq %r15\npopq %r14\npopq %r13\npopq %r12\npopq %rbx\npopq %rbp\nret\n"
);
