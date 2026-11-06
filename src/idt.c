#include "idt.h"
#include "xstate.h"
#include "mmu.h"
#include "proof_sched.h"
#include "uart.h"
#include "runtime.h"
static idt_entry_t idt[256] ALIGN_4K __attribute__((section(".bss.idt")));
typedef char gate_size[sizeof(idt_entry_t)==16?1:-1];
typedef struct __attribute__((packed)) { uint16_t limit; uint64_t base; } table_ptr;
typedef struct __attribute__((packed)) {
 uint32_t reserved0; uint64_t rsp[3]; uint64_t reserved1;
 uint64_t ist[7]; uint64_t reserved2; uint16_t reserved3,iomap;
} tss_t;
static uint64_t cpu_gdt[2][7] ALIGN_64;
static tss_t tss[2] ALIGN_64;
static uint8_t privilege_stack[2][16384] ALIGN_4K;
static uint8_t emergency_stack[2][8192] ALIGN_4K;
uint64_t user_kernel_rsp __attribute__((used));
static uint64_t fault_task;
static unsigned observed_faults;
static uint64_t observed_vector,observed_error,observed_address;
extern void enter_user(uint64_t rip,uint64_t rsp);
extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr15(void);
extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr22(void);
extern void isr23(void);
extern void isr24(void);
extern void isr25(void);
extern void isr26(void);
extern void isr27(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);
extern void isr255(void);
extern void isr48(void);
__asm__(
".text\n"
".global exception_common\n"
"exception_common:\n"
"cld\n"
"pushq %rax\n"
"pushq %rbx\n"
"pushq %rcx\n"
"pushq %rdx\n"
"pushq %rbp\n"
"pushq %rsi\n"
"pushq %rdi\n"
"pushq %r8\n"
"pushq %r9\n"
"pushq %r10\n"
"pushq %r11\n"
"pushq %r12\n"
"pushq %r13\n"
"pushq %r14\n"
"pushq %r15\n"
"movq %rsp,%rdi\n"
"movq %rsp,%rbx\n"
"andq $-16,%rsp\n"
"call exception_dispatch\n"
"movq %rbx,%rsp\n"
"cmpq $2,%rax\n"
"je runtime_return\n"
"testq %rax,%rax\n"
"jnz user_abort\n"
"popq %r15\n"
"popq %r14\n"
"popq %r13\n"
"popq %r12\n"
"popq %r11\n"
"popq %r10\n"
"popq %r9\n"
"popq %r8\n"
"popq %rdi\n"
"popq %rsi\n"
"popq %rbp\n"
"popq %rdx\n"
"popq %rcx\n"
"popq %rbx\n"
"popq %rax\n"
"addq $16,%rsp\n"
"iretq\n"
".global isr0\n"
"isr0:\n"
"pushq $0\n"
"pushq $0\n"
"jmp exception_common\n"
".global isr1\n"
"isr1:\n"
"pushq $0\n"
"pushq $1\n"
"jmp exception_common\n"
".global isr2\n"
"isr2:\n"
"pushq $0\n"
"pushq $2\n"
"jmp exception_common\n"
".global isr3\n"
"isr3:\n"
"pushq $0\n"
"pushq $3\n"
"jmp exception_common\n"
".global isr4\n"
"isr4:\n"
"pushq $0\n"
"pushq $4\n"
"jmp exception_common\n"
".global isr5\n"
"isr5:\n"
"pushq $0\n"
"pushq $5\n"
"jmp exception_common\n"
".global isr6\n"
"isr6:\n"
"pushq $0\n"
"pushq $6\n"
"jmp exception_common\n"
".global isr7\n"
"isr7:\n"
"pushq $0\n"
"pushq $7\n"
"jmp exception_common\n"
".global isr8\n"
"isr8:\n"
"pushq $8\n"
"jmp exception_common\n"
".global isr9\n"
"isr9:\n"
"pushq $0\n"
"pushq $9\n"
"jmp exception_common\n"
".global isr10\n"
"isr10:\n"
"pushq $10\n"
"jmp exception_common\n"
".global isr11\n"
"isr11:\n"
"pushq $11\n"
"jmp exception_common\n"
".global isr12\n"
"isr12:\n"
"pushq $12\n"
"jmp exception_common\n"
".global isr13\n"
"isr13:\n"
"pushq $13\n"
"jmp exception_common\n"
".global isr14\n"
"isr14:\n"
"pushq $14\n"
"jmp exception_common\n"
".global isr15\n"
"isr15:\n"
"pushq $0\n"
"pushq $15\n"
"jmp exception_common\n"
".global isr16\n"
"isr16:\n"
"pushq $0\n"
"pushq $16\n"
"jmp exception_common\n"
".global isr17\n"
"isr17:\n"
"pushq $17\n"
"jmp exception_common\n"
".global isr18\n"
"isr18:\n"
"pushq $0\n"
"pushq $18\n"
"jmp exception_common\n"
".global isr19\n"
"isr19:\n"
"pushq $0\n"
"pushq $19\n"
"jmp exception_common\n"
".global isr20\n"
"isr20:\n"
"pushq $0\n"
"pushq $20\n"
"jmp exception_common\n"
".global isr21\n"
"isr21:\n"
"pushq $21\n"
"jmp exception_common\n"
".global isr22\n"
"isr22:\n"
"pushq $0\n"
"pushq $22\n"
"jmp exception_common\n"
".global isr23\n"
"isr23:\n"
"pushq $0\n"
"pushq $23\n"
"jmp exception_common\n"
".global isr24\n"
"isr24:\n"
"pushq $0\n"
"pushq $24\n"
"jmp exception_common\n"
".global isr25\n"
"isr25:\n"
"pushq $0\n"
"pushq $25\n"
"jmp exception_common\n"
".global isr26\n"
"isr26:\n"
"pushq $0\n"
"pushq $26\n"
"jmp exception_common\n"
".global isr27\n"
"isr27:\n"
"pushq $0\n"
"pushq $27\n"
"jmp exception_common\n"
".global isr28\n"
"isr28:\n"
"pushq $0\n"
"pushq $28\n"
"jmp exception_common\n"
".global isr29\n"
"isr29:\n"
"pushq $29\n"
"jmp exception_common\n"
".global isr30\n"
"isr30:\n"
"pushq $30\n"
"jmp exception_common\n"
".global isr31\n"
"isr31:\n"
"pushq $0\n"
"pushq $31\n"
"jmp exception_common\n"
".global isr48\nisr48: pushq $0\npushq $48\njmp exception_common\n"
".global isr255\n"
"isr255:\n"
"pushq $0\n"
"pushq $255\n"
"jmp exception_common\n"
".global enter_user\n"
"enter_user:\n"
"pushq %rbp\n"
"pushq %rbx\n"
"pushq %r12\n"
"pushq %r13\n"
"pushq %r14\n"
"pushq %r15\n"
"movq %rsp,user_kernel_rsp\n"
"pushq $0x1b\n"
"pushq %rsi\n"
"pushq $2\n"
"pushq $0x23\n"
"pushq %rdi\n"
"iretq\n"
"user_abort:\n"
"movq user_kernel_rsp,%rsp\n"
"popq %r15\n"
"popq %r14\n"
"popq %r13\n"
"popq %r12\n"
"popq %rbx\n"
"popq %rbp\n"
"ret\n"
);
static void (*const stubs[32])(void)={isr0,isr1,isr2,isr3,isr4,isr5,isr6,isr7,isr8,isr9,isr10,isr11,isr12,isr13,isr14,isr15,isr16,isr17,isr18,isr19,isr20,isr21,isr22,isr23,isr24,isr25,isr26,isr27,isr28,isr29,isr30,isr31};

void idt_init_cpu(unsigned cpu) {
 xstate_init_cpu();
 uint64_t base; table_ptr gdtr,idtr; uint16_t selector=0x28;
 if(cpu>1) panic("TSS CPU bounds");
 cpu_gdt[cpu][0]=0; cpu_gdt[cpu][1]=0x00af9a000000ffffULL; cpu_gdt[cpu][2]=0x00cf92000000ffffULL;
 cpu_gdt[cpu][3]=0x00cff2000000ffffULL; cpu_gdt[cpu][4]=0x00affa000000ffffULL;
 memset(&tss[cpu],0,sizeof(tss[cpu])); tss[cpu].rsp[0]=(uint64_t)&privilege_stack[cpu][16384];
 tss[cpu].ist[0]=(uint64_t)&emergency_stack[cpu][8192]; tss[cpu].iomap=sizeof(tss_t);
 base=(uint64_t)&tss[cpu];
 cpu_gdt[cpu][5]=(sizeof(tss_t)-1)|((base&0xffffff)<<16)|(0x89ULL<<40)|((base>>24&255)<<56);
 cpu_gdt[cpu][6]=base>>32;
 gdtr.limit=sizeof(cpu_gdt[cpu])-1; gdtr.base=(uint64_t)cpu_gdt[cpu];
 idtr.limit=sizeof(idt)-1; idtr.base=(uint64_t)idt;
 __asm__ volatile("lgdt %0"::"m"(gdtr):"memory");
 /* AP arrived with trampoline CS=0x18; reload its hidden descriptor before
  * replacing that selector with user data in the per-CPU GDT. */
 __asm__ volatile("pushq $8; leaq 1f(%%rip),%%rax; pushq %%rax; lretq; 1:\n"
                  "movw $16,%%ax; movw %%ax,%%ds; movw %%ax,%%es; movw %%ax,%%ss"
                  :::"rax","memory");
 __asm__ volatile("ltr %0; lidt %1"::"r"(selector),"m"(idtr):"memory");
}
void idt_init(void) {
 unsigned i;
 for(i=0;i<256;i++) {
  uint64_t addr=(uint64_t)(i<32?stubs[i]:(i==48?isr48:isr255));
  idt[i].offset_lo=(uint16_t)addr; idt[i].selector=8;
  idt[i].ist=(uint8_t)((i==8||i==2)?1:0); idt[i].attributes=0x8e;
  idt[i].offset_mid=(uint16_t)(addr>>16); idt[i].offset_hi=(uint32_t)(addr>>32); idt[i].reserved=0;
 }
 /* Mask legacy PIC: no IRQ handler or interrupt scheduler is installed yet. */
 outb(0x21,255); outb(0xa1,255); idt_init_cpu(0);
 uart_puts("IDT 256 gates / 32 exception stubs / TSS loaded\n");
}
uint64_t exception_dispatch(interrupt_frame_t *f) {
 if(runtime_active()) return runtime_trap(f);
 uint64_t cr2; __asm__ volatile("movq %%cr2,%0":"=r"(cr2));
 if((f->cs&3)==3 && fault_task && (f->vector==13||f->vector==14)) {
  omega_task_t *task=(omega_task_t *)fault_task;
  observed_vector=f->vector; observed_error=f->error; observed_address=cr2;
  uart_puts("USER fault quarantined vector="); uart_put_dec(f->vector);
  uart_puts(" CR2="); uart_put_hex(cr2); uart_puts("\n");
  if(!sls_restore(fault_task)||!omega_verify_contract(task)) panic("disk rollback invalid");
  task->quarantined=true; observed_faults++;
  /* Abandon faulting user context. Common stub restores the saved kernel stack
   * and callee-saved registers; it never retries a malicious instruction. */
  return 1;
 }
 uart_puts("KERNEL exception vector="); uart_put_dec(f->vector);
 uart_puts(" error="); uart_put_hex(f->error); uart_puts(" RIP="); uart_put_hex(f->rip);
 uart_puts(" CR2="); uart_put_hex(cr2); uart_puts("\n"); panic("unrecoverable kernel exception");
}
void isolation_test(uint64_t task_va) {
 uint64_t code=pmm_alloc_frame(),stack=pmm_alloc_frame(); uint8_t *bytes=(uint8_t *)code;
 omega_task_t *task=(omega_task_t *)task_va; int64_t expected_num=task->state[0].num;
 if(!code||!stack) panic("user frame allocation");
 if(!mmu_map(boot_pml4,0x50000000,code,PAGE_USER)||
    !mmu_map(boot_pml4,0x50001000,stack,PAGE_USER|PAGE_WRITE)) panic("user page mapping");
 fault_task=task_va;
 /* User instruction: movabs $0x100000,%rax; mov (%rax),%rax. Supervisor page. */
 bytes[0]=0x48; bytes[1]=0xb8;
 { uint64_t target=0x100000; memcpy(bytes+2,&target,8); }
 bytes[10]=0x48; bytes[11]=0x8b; bytes[12]=0x00; bytes[13]=0xeb; bytes[14]=0xfe;
 task->state[0].num=99;
 compiler_barrier();
 enter_user(0x50000000,0x50002000);
 if(observed_vector!=14||observed_address!=0x100000||(observed_error&7)!=5||!task->quarantined||task->state[0].num!=expected_num) panic("PF quarantine/rollback missing");
 if(!sls_restore(task_va)||!omega_verify_contract(task)) panic("PF rollback");
 /* User OUT DX,AL with IOPL=0 and no TSS I/O bitmap: must raise #GP. */
 bytes[0]=0xba; bytes[1]=0xf8; bytes[2]=0x03; bytes[3]=0; bytes[4]=0;
 bytes[5]=0xee; bytes[6]=0xeb; bytes[7]=0xfe;
 task->state[0].num=99;
 compiler_barrier(); enter_user(0x50000000,0x50002000);
 if(observed_vector!=13||observed_faults!=2||!task->quarantined||task->state[0].num==99) panic("GP quarantine/rollback missing");
 if(!sls_restore(task_va)) panic("CR3 test restore");
 /* MOV CR3,RAX is privileged independently of the port-I/O protection. */
 bytes[0]=0x0f; bytes[1]=0x20; bytes[2]=0xd8; bytes[3]=0xeb; bytes[4]=0xfe;
 task->state[0].num=99; compiler_barrier(); enter_user(0x50000000,0x50002000);
 if(observed_vector!=13||observed_faults!=3||!task->quarantined||task->state[0].num==99) panic("CR3 privilege/rollback missing");
 uart_puts("TEST Ring3 CR3 privilege GP PASS\n");
 if(!sls_restore(task_va)) panic("unmapped PF test restore");
 bytes[0]=0x48; bytes[1]=0xb8;
 { uint64_t target=0x60000000; memcpy(bytes+2,&target,8); }
 bytes[10]=0x48; bytes[11]=0x8b; bytes[12]=0; bytes[13]=0xeb; bytes[14]=0xfe;
 task->state[0].num=99; compiler_barrier(); enter_user(0x50000000,0x50002000);
 if(observed_faults!=4||observed_vector!=14||observed_address!=0x60000000||
    (observed_error&7)!=4||!task->quarantined||task->state[0].num==99) panic("unmapped user PF rollback");
 uart_puts("TEST Ring3 unmapped PF PASS\n");
 fault_task=0; uart_puts("TEST Ring3 PF/GP + disk rollback PASS\n");
}
