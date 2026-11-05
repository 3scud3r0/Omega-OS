#include "runtime.h"
#include "mmu.h"
#include "ipc.h"
#include "uart.h"
#include "admission.h"
#include "smp.h"
#include "net.h"
#define TASKS 5U
#define USER_CODE 0x80000000ULL
#define USER_STACK 0x80001000ULL
#define USER_DATA 0x80002000ULL
static struct {
 uint64_t root,data,tx,ticks,calls,fault,address,error; interrupt_frame_t saved; bool dead;
} process[TASKS] ALIGN_64;
static omega_ring_t messages ALIGN_64;
static unsigned current,completed,preemptions;
static bool running;
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
  if(process[current].ticks>=8) { process[current].dead=true; completed++; }
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
  case 4: if(f->rdi>=14&&f->rdi<=1514&&net_send_page((void *)process[current].tx,(size_t)f->rdi)) result=0; break;
  case 7: result=net_mac(); break;
  default: break;
 }
 f->rax=result;
 /* RIP/RFLAGS are never used blindly by SYSRET.
  * Validated low-canonical RIP/RSP permit the SYSRET fast return. */
 if(f->rip<USER_CODE||f->rip>=USER_CODE+4096||f->rsp<USER_STACK||f->rsp>USER_DATA) {
  process[current].fault=128; process[current].address=f->rsp;
  process[current].dead=true; completed++;
  return choose(f)?0:2;
 }
 f->rflags=(f->rflags&0x8d5ULL)|0x202; return 3;
}
void runtime_run(void) {
 const uint8_t *code; size_t size; unsigned i; uint64_t start=rdtsc();
 code=admission_code(&size); ring_init(&messages);
 { uint32_t lo,hi; __asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(0xc0000080)); wrmsr(0xc0000080,((uint64_t)hi<<32)|lo|0x800); }
 for(i=0;i<TASKS;i++) {
  uint64_t text=pmm_alloc_frame(),stack=pmm_alloc_frame(); interrupt_frame_t *f=&process[i].saved;
  process[i].root=create_root(); process[i].data=pmm_alloc_frame(); process[i].tx=pmm_alloc_frame();
  if(!text||!stack||!process[i].data||!process[i].tx) panic("process memory exhausted");
  memcpy((void *)text,code,size); *(uint64_t *)process[i].data=i;
  if(!mmu_map((uint64_t *)process[i].root,USER_CODE,text,PAGE_USER)||
     !mmu_map((uint64_t *)process[i].root,USER_STACK,stack,PAGE_USER|PAGE_WRITE|PAGE_NX)||
     !mmu_map((uint64_t *)process[i].root,USER_DATA,process[i].data,PAGE_USER|PAGE_WRITE|PAGE_NX)||
     !mmu_map((uint64_t *)process[i].root,USER_DATA+4096,process[i].tx,PAGE_USER|PAGE_WRITE|PAGE_NX)) panic("private user mapping");
  /* Only process 0 can read this marker; process 2's access must fault. */
  if(i==0&&!mmu_map((uint64_t *)process[i].root,0x90000000,process[i].data,PAGE_USER)) panic("private marker");
  f->rip=USER_CODE; f->rsp=USER_DATA; f->cs=USER_CS; f->ss=USER_DS; f->rflags=0x202;
 }
 syscall_stack_top=(uint64_t)&syscall_kernel_stack[2048];
 wrmsr(0xc0000081,((uint64_t)0x13<<48)|((uint64_t)8<<32));
 wrmsr(0xc0000082,(uint64_t)syscall_entry); wrmsr(0xc0000084,0x47700);
 { uint32_t lo,hi; __asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(0xc0000080)); wrmsr(0xc0000080,((uint64_t)hi<<32)|lo|1); }
 running=true; current=0; switch_root(process[0].root); smp_timer_start();
 runtime_enter(&process[0].saved);
 if(completed!=TASKS||preemptions<8||!*(uint64_t *)(process[0].data+8)||*(uint64_t *)(process[1].data+8)!=256||process[1].calls<257) panic("runtime completion");
 if(process[4].fault!=128||process[4].address!=0xffff800000000000ULL||process[0].ticks!=8||process[2].fault!=14||process[2].address!=0x90000000||(process[2].error&7)!=4||
    process[3].fault!=14||process[3].address!=USER_STACK||(process[3].error&0x15)!=0x15) panic("process isolation/NX fault evidence");
 uart_puts("TEST noncanonical syscall stack quarantined without SYSRET PASS\n");
 uart_puts("TEST private peer page PF + non-executable stack PF PASS\n");
 uart_puts("TEST preemption infinite agent revoked; private CR3; Ring3 IPC syscalls PASS\n");
 uart_puts("SYSCALL/SYSRET mean TSC ticks="); uart_put_dec(*(uint64_t *)(process[1].data+16)); uart_puts("\n");
 uart_puts("SYSCALL runtime total TSC ticks="); uart_put_dec(rdtsc()-start); uart_puts("\n");
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
