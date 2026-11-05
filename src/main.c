#include "uart.h"
#include "mmu.h"
#include "ipc.h"
#include "proof_sched.h"
/* No external assembler: .code32 trampoline and .code64 C entry in this TU. */
__asm__(
".section .multiboot,\"a\"\n.balign 4\n"
".long 0x1badb002,3,-(0x1badb002+3)\n"
".section .bss.boot,\"aw\",@nobits\n.balign 4096\n"
".global boot_pml4\nboot_pml4: .skip 4096\n"
"boot_pdpt: .skip 4096\nboot_pd: .skip 4096\nboot_pts: .skip 131072\n"
".balign 16\nboot_stack: .skip 16384\nboot_stack_top:\n"
".section .rodata\n.balign 8\ngdt:\n"
".quad 0,0x00af9a000000ffff,0x00cf92000000ffff\n"
"gdt_end:\ngdt_ptr: .word gdt_end-gdt-1\n.long gdt\n"
".section .text.boot,\"ax\"\n.code32\n.global _start\n_start:\n"
"cli\ncld\nmovl %eax,%edx\nmovl %ebx,%ebp\n"
"movl $__bss_start,%edi\nmovl $__kernel_end,%ecx\nsubl %edi,%ecx\nshrl $2,%ecx\nxorl %eax,%eax\nrep stosl\n"
"movl $boot_pdpt,%eax\norl $3,%eax\nmovl %eax,boot_pml4\n"
"movl $boot_pd,%eax\norl $3,%eax\nmovl %eax,boot_pdpt\n"
"xorl %ecx,%ecx\n1:\nmovl %ecx,%eax\nshll $12,%eax\naddl $boot_pts,%eax\norl $3,%eax\nmovl %eax,boot_pd(,%ecx,8)\nincl %ecx\ncmpl $32,%ecx\njne 1b\n"
"xorl %ecx,%ecx\n2:\nmovl %ecx,%eax\nshll $12,%eax\norl $3,%eax\nmovl %eax,boot_pts(,%ecx,8)\nincl %ecx\ncmpl $16384,%ecx\njne 2b\n"
"movl %edx,%edi\nmovl %ebp,%esi\n"
"movl %cr4,%eax\norl $0x20,%eax\nmovl %eax,%cr4\n"
"movl $boot_pml4,%eax\nmovl %eax,%cr3\n"
"movl $0xc0000080,%ecx\nrdmsr\norl $0x100,%eax\nwrmsr\n"
"lgdt gdt_ptr\nmovl %cr0,%eax\norl $0x80010000,%eax\nmovl %eax,%cr0\n"
"ljmp $8,$long_entry\n.code64\nlong_entry:\n"
"movw $16,%ax\nmovw %ax,%ds\nmovw %ax,%es\nmovw %ax,%ss\n"
"movq $boot_stack_top,%rsp\nxorq %rbp,%rbp\ncall kmain\n3: cli\nhlt\njmp 3b\n"
".section .note.GNU-stack,\"\",@progbits\n.text\n");
extern uint8_t __kernel_end[];
static omega_ring_t channel ALIGN_64;
void *memset(void *p,int c,size_t n) { uint8_t *b=p; size_t i; for(i=0;i<n;i++) b[i]=(uint8_t)c; return p; }
void *memcpy(void *d,const void *s,size_t n) { uint8_t *a=d; const uint8_t *b=s; size_t i; for(i=0;i<n;i++) a[i]=b[i]; return d; }
void panic(const char *why) { uart_puts("FAIL: "); uart_puts(why); uart_puts("\n"); for(;;) __asm__ volatile("cli; hlt"); }
static void require(bool ok,const char *why) { if(!ok) panic(why); }
static void rational_tests(void) {
 omega_rational_t a,b,r; int c;
 require(rational_make(2,-4,&a)&&a.num==-1&&a.den==2,"normalize");
 require(rational_make(1,3,&b),"rational create");
 require(rational_add(a,b,&r)&&r.num==-1&&r.den==6,"rational add");
 require(rational_sub(a,b,&r)&&r.num==-5&&r.den==6,"rational sub");
 require(rational_mul(a,b,&r)&&r.num==-1&&r.den==6,"rational mul");
 require(rational_cmp(a,b,&c)&&c<0,"rational compare");
 require(!rational_make(1,0,&r),"zero denominator");
 a.num=9223372036854775807LL; a.den=1; b=a;
 require(!rational_add(a,b,&r),"overflow rejection");
 b.num=(-9223372036854775807LL-1); require(rational_cmp(a,b,&c)&&c>0,"extreme compare");
 uart_puts("TEST rational arithmetic/overflow PASS\n");
}
static void ring_tests(void) {
 uint64_t v; unsigned i;
 ring_init(&channel); require(!ring_pop(&channel,&v),"empty ring");
 /* Exercise full capacity and several wraps of slot indices. */
 for(unsigned round=0;round<3;round++) {
  for(i=0;i<RING_CAPACITY;i++) require(ring_push(&channel,i),"ring fill");
  require(!ring_push(&channel,0),"ring full");
  for(i=0;i<RING_CAPACITY;i++) require(ring_pop(&channel,&v)&&v==i,"FIFO");
 }
 channel.head=~0ULL-511; channel.tail=channel.head;
 for(i=0;i<1024;i++) { require(ring_push(&channel,i),"counter wrap push"); require(ring_pop(&channel,&v)&&v==i,"counter wrap pop"); }
 ring_init(&channel); uart_puts("TEST SPSC full/empty/FIFO/wrap PASS\n");
}
static void init_task(omega_task_t *t,uint64_t id) {
 unsigned i; t->id=id;
 for(i=0;i<STATE_DIM;i++) { t->state[i].num=0; t->state[i].den=1; }
 t->proof_hash=omega_contract_token(t);
}
void kmain(uint32_t magic,uint32_t info) __attribute__((used,externally_visible));
void kmain(uint32_t magic,uint32_t info) {
 uint64_t phys[4],cycles,start,dirty,value; unsigned i,step;
 omega_task_t *tasks[2],*saved[2]; uint64_t available;
 uart_init(); uart_puts("[OMEGA-OS] BOOT x86-64 / 4-level paging\n");
 require(magic==0x2badb002,"Multiboot magic");
 require(info<PHYS_LIMIT-12,"Multiboot pointer");
 { const uint32_t *mb=(const uint32_t *)(uint64_t)info;
  require((mb[0]&1)!=0,"Multiboot memory information"); available=0x100000ULL+(uint64_t)mb[2]*1024;
 }
 /* Everything at/below kernel end is reserved. Boot info is consumed above. */
 pmm_init(available,(uint64_t)__kernel_end); uart_puts("PMM 64 MiB cap initialized\n");
 for(i=0;i<4;i++) { phys[i]=pmm_alloc_frame(); require(phys[i]!=0,"PMM allocation"); uart_puts("PMM allocated "); uart_put_hex(phys[i]); uart_puts("\n"); }
 { uint64_t f=pmm_alloc_frame(); require(f&&pmm_free_frame(f)&&!pmm_free_frame(f),"frame free/double-free"); require(pmm_alloc_frame()==f,"frame reuse"); require(pmm_free_frame(f),"frame release"); }
 require(!pmm_free_frame(0)&&!pmm_free_frame(PHYS_LIMIT),"PMM reserved bounds");
 for(i=0;i<2;i++) {
  uint64_t va=0x40000000ULL+(uint64_t)i*4096;
  require(mmu_map(boot_pml4,va,phys[i],PAGE_WRITE),"map SLS task");
  tasks[i]=(omega_task_t *)va; saved[i]=(omega_task_t *)phys[2+i];
  init_task(tasks[i],i+1); require(omega_checkpoint(tasks[i],saved[i]),"initial checkpoint");
 }
 rational_tests(); ring_tests();
 start=rdtsc();
 /* Cooperative producer/consumer on BSP: no AP startup is claimed. */
 for(step=0;step<16;step++) {
  require(omega_admit(tasks[0],saved[0]),"producer admission");
  require(rational_make((int64_t)(step%4),8,&tasks[0]->state[0]),"producer state");
  tasks[0]->epoch++; tasks[0]->proof_hash=omega_contract_token(tasks[0]);
  require(omega_admit(tasks[0],saved[0]),"producer contract");
  require(ring_push(&channel,step),"producer send");
  require(omega_admit(tasks[1],saved[1]),"consumer admission");
  require(ring_pop(&channel,&value)&&value==step,"consumer receive");
  require(rational_make((int64_t)(value%4),8,&tasks[1]->state[0]),"consumer state");
  tasks[1]->epoch++; tasks[1]->proof_hash=omega_contract_token(tasks[1]);
  require(omega_admit(tasks[1],saved[1]),"consumer contract");
  for(i=0;i<2;i++) require(omega_checkpoint(tasks[i],saved[i]),"commit checkpoint");
  dirty=mmu_sls_commit(boot_pml4); require(dirty>=2,"hardware dirty tracking");
 }
 cycles=rdtsc()-start;
 uart_puts("IPC transferred 16 messages; contracts accepted\n");
 /* Corruption injection must restore the entire frame, then quarantine. */
 tasks[1]->state[0].num=99;
 require(!omega_admit(tasks[1],saved[1])&&tasks[1]->quarantined,"quarantine");
 require(tasks[1]->state[0].num==saved[1]->state[0].num,"rollback value");
 require(!omega_verify_contract(tasks[1]),"quarantined admission");
 /* Valid tag does not authorize an invalid norm. */
 tasks[0]->state[0].num=2; tasks[0]->state[0].den=1;
 tasks[0]->proof_hash=omega_contract_token(tasks[0]);
 require(!omega_admit(tasks[0],saved[0]),"norm violation");
 uart_puts("TEST contract/tag/norm/quarantine/frame rollback PASS\nCPU TSC cycles: "); uart_put_dec(cycles);
 uart_puts("\nSTABLE: 16 cooperative cycles completed; RAM checkpoints only; entering HLT\n");
 for(;;) __asm__ volatile("cli; hlt");
}
