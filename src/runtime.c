#include "runtime.h"
#include "mmu.h"
#include "ipc.h"
#include "uart.h"
#include "admission.h"
#include "smp.h"
#include "net.h"
#include "udp.h"
#include "bytecode.h"
#include "crypto/monocypher.h"
#define TASKS 7U
#define USER_CODE 0x80000000ULL
#define USER_STACK 0x80001000ULL
#define USER_DATA 0x80002000ULL
static struct {
 uint64_t root,data,tx,ticks,calls,fault,address,error,pc_low,pc_high,sp_low,sp_high; interrupt_frame_t saved; bool dead;
} process[TASKS] ALIGN_64;
static omega_ring_t messages ALIGN_64;
static unsigned current,completed,preemptions;
static bool running,mutation_ready,child_started,compiler_reported,child_reported;
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
  if(!process[next].dead) {
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
    spawn_child((void *)process[current].tx,(size_t)f->rdi); result=7;
   }
   break;
  case 9:
   if(current==0&&f->rdi>1&&f->rdi<=512&&!mutation_ready) {
    memcpy((void *)process[5].tx,(void *)process[0].tx,(size_t)f->rdi);
    ((char *)process[5].tx)[f->rdi-1]=0;mutation_ready=true;result=0;
   }
   break;
  case 10: result=mutation_ready;break;
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
 if(f->rip<process[current].pc_low||f->rip>=process[current].pc_high||f->rsp<process[current].sp_low||f->rsp>process[current].sp_high) {
  process[current].fault=128; process[current].address=f->rsp;
  process[current].dead=true; completed++;
  return choose(f)?0:2;
 }
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
 process[id].root=create_root();process[id].data=pmm_alloc_frame();process[id].tx=pmm_alloc_frame();
 if(!process[id].data||!process[id].tx)panic("compiler metadata frames");
 *(uint64_t *)process[id].data=role;
 if(!mmu_map((uint64_t *)process[id].root,USER_DATA,process[id].data,PAGE_USER|PAGE_WRITE|PAGE_NX)||
    !mmu_map((uint64_t *)process[id].root,USER_DATA+4096,process[id].tx,PAGE_USER|(id==6?0:PAGE_WRITE)|PAGE_NX))panic("compiler IO pages");
 for(uint64_t va=0x80f00000;va<0x80f10000;va+=4096){uint64_t p=pmm_alloc_frame();if(!p||!mmu_map((uint64_t *)process[id].root,va,p,PAGE_USER|PAGE_WRITE|PAGE_NX))panic("compiler stack");}
 if(id==5&&!mmu_map((uint64_t *)process[id].root,0x80004000,cognitive_phys,PAGE_USER|PAGE_WRITE|PAGE_NX))panic("explicit SLS page capability");
 process[id].sp_low=0x80f00000;process[id].sp_high=0x80f10000;
 f->rip=load_elf(id);f->rsp=process[id].sp_high;f->cs=USER_CS;f->ss=USER_DS;f->rflags=0x202;
}
static void spawn_child(const void *capsule,size_t n) {
 compiler_context(6,6);memcpy((void *)process[6].tx,capsule,n);((uint64_t *)process[6].data)[4]=n;
 child_started=true;process[6].dead=false;
 uart_puts("ADMISSION delegated rational bytecode capsule -> fresh Ring3 PID 7\n");
}
bool runtime_run(void) {
 const uint8_t *code; size_t size; unsigned i; uint64_t start=rdtsc();
 code=admission_code(&size); ring_init(&messages);
 { uint32_t lo,hi; __asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(0xc0000080)); wrmsr(0xc0000080,((uint64_t)hi<<32)|lo|0x800); }
 for(i=0;i<5;i++) {
  uint64_t text=pmm_alloc_frame(),stack=pmm_alloc_frame(); interrupt_frame_t *f=&process[i].saved;
  process[i].pc_low=USER_CODE;process[i].pc_high=USER_CODE+4096;process[i].sp_low=USER_STACK;process[i].sp_high=USER_DATA;
  process[i].root=create_root(); process[i].data=pmm_alloc_frame(); process[i].tx=pmm_alloc_frame();
  if(!text||!stack||!process[i].data||!process[i].tx) panic("process memory exhausted");
  memcpy((void *)text,code,size); *(uint64_t *)process[i].data=i;
  if(!mmu_map((uint64_t *)process[i].root,USER_CODE,text,PAGE_USER)||
     !mmu_map((uint64_t *)process[i].root,USER_STACK,stack,PAGE_USER|PAGE_WRITE|PAGE_NX)||
     !mmu_map((uint64_t *)process[i].root,USER_DATA,process[i].data,PAGE_USER|PAGE_WRITE|PAGE_NX)||
     !mmu_map((uint64_t *)process[i].root,USER_DATA+4096,process[i].tx,PAGE_USER|PAGE_WRITE|PAGE_NX)) panic("private user mapping");
  if(i==0&&requested_size){memcpy((void *)process[0].tx,requested_source,requested_size);((uint64_t *)process[0].data)[21]=requested_size;}
  /* Only process 0 can read this marker; process 2's access must fault. */
  if(i==0&&!mmu_map((uint64_t *)process[i].root,0x90000000,process[i].data,PAGE_USER)) panic("private marker");
  f->rip=USER_CODE; f->rsp=USER_DATA; f->cs=USER_CS; f->ss=USER_DS; f->rflags=0x202;
 }
 {uint64_t p=pmm_alloc_frame();if(!p||!mmu_map(boot_pml4,(uint64_t)cognitive,p,PAGE_WRITE))panic("cognitive SLS frame");cognitive_phys=p;(void)sls_register((uint64_t)cognitive);}
 compiler_image=admission_compiler(&compiler_size);compiler_context(5,5);process[6].dead=true;
 syscall_stack_top=(uint64_t)&syscall_kernel_stack[2048];
 wrmsr(0xc0000081,((uint64_t)0x13<<48)|((uint64_t)8<<32));
 wrmsr(0xc0000082,(uint64_t)syscall_entry); wrmsr(0xc0000084,0x47700);
 { uint32_t lo,hi; __asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(0xc0000080)); wrmsr(0xc0000080,((uint64_t)hi<<32)|lo|1); }
 running=true; current=0; switch_root(process[0].root); smp_timer_start();
 runtime_enter(&process[0].saved);
 if(completed!=(child_started?TASKS:TASKS-1)||preemptions<8||!*(uint64_t *)(process[0].data+8)||*(uint64_t *)(process[1].data+8)!=256||process[1].calls<257) panic("runtime completion");
 if(process[4].fault!=128||process[4].address!=0xffff800000000000ULL||process[0].ticks!=8||process[2].fault!=14||process[2].address!=0x90000000||(process[2].error&7)!=4||
    process[3].fault!=14||process[3].address!=USER_STACK||(process[3].error&0x15)!=0x15) panic("process isolation/NX fault evidence");
 if(!compiler_reported||!child_reported||!child_started){
  (void)sls_restore((uint64_t)cognitive);
  uart_puts("ADMISSION rejected mutation; kernel retained prior SLS checkpoint\n");return false;
 }
 uart_puts("TEST original Omega frontend + egraph + tensor PUCT in Ring3 + dynamic child + exact SLS PASS\n");
 uart_puts("TEST noncanonical syscall stack quarantined without SYSRET PASS\n");
 uart_puts("TEST private peer page PF + non-executable stack PF PASS\n");
 uart_puts("TEST preemption infinite agent revoked; private CR3; Ring3 IPC syscalls PASS\n");
 uart_puts("SYSCALL/SYSRET mean TSC ticks="); uart_put_dec(*(uint64_t *)(process[1].data+16)); uart_puts("\n");
 uart_puts("SYSCALL runtime total TSC ticks="); uart_put_dec(rdtsc()-start); uart_puts("\n");return true;
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
