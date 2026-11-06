#include "uart.h"
#include "mmu.h"
#include "ipc.h"
#include "proof_sched.h"
#include "idt.h"
#include "virtio_blk.h"
#include "smp.h"
#include "runtime.h"
#include "consensus.h"
#include "net.h"
#include "udp.h"
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
"movl $0xc0000080,%ecx\nrdmsr\norl $0x900,%eax\nwrmsr\n"
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
 uint64_t phys[3],available,start,cycles,dirty; unsigned i; bool crash_test=false,stress=false;
 omega_task_t *tasks[2]; uint8_t *probe=(uint8_t *)0x40002000;
 uart_init(); uart_puts("[OMEGA-OS V4] BOOT x86-64\n");
 idt_init();
 require(magic==0x2badb002&&info<PHYS_LIMIT-32,"Multiboot data");
 { const uint32_t *mb=(const uint32_t *)(uint64_t)info;
  require((mb[0]&1)!=0,"Multiboot memory information"); available=0x100000ULL+(uint64_t)mb[2]*1024;
  if((mb[0]&4)&&mb[4]&&mb[4]<PHYS_LIMIT-32) {
   const char *cmd=(const char *)(uint64_t)mb[4];
   const char prefix[]="sls.crash="; unsigned pos;
   for(pos=0;pos<256&&cmd[pos];pos++) {
    unsigned j=0;
    if((uint64_t)mb[4]+pos+16>=PHYS_LIMIT) break;
    while(j<10&&cmd[pos+j]==prefix[j]) j++;
    {const char key[]="sls.stress";unsigned k=0;while(k<10&&cmd[pos+k]==key[k])k++;if(k==10)stress=true;}
    {const char key[]="omega.source=";unsigned k=0;while(k<13&&cmd[pos+k]==key[k])k++;
     if(k==13){size_t n=0;while(n<511&&(uint64_t)mb[4]+pos+k+n<PHYS_LIMIT&&cmd[pos+k+n])n++;runtime_source(cmd+pos+k,n);break;}}
    if(j==10&&cmd[pos+j]>='1'&&cmd[pos+j]<='8') { sls_test_crash_after((unsigned)(cmd[pos+j]-'0')); crash_test=true; }
   }
  }
 }
 net_boot_config(info);
 pmm_init(available,(uint64_t)__kernel_end);
 uart_puts("PMM 64 MiB cap initialized\n");
 { uint64_t frame=pmm_alloc_frame();
  require(frame&&pmm_free_frame(frame)&&!pmm_free_frame(frame),"PMM free/double free");
  require(pmm_alloc_frame()==frame&&pmm_free_frame(frame),"PMM frame reuse");
  require(!pmm_free_frame(0)&&!pmm_free_frame(PHYS_LIMIT),"PMM reserved bounds");
 }
 virtio_blk_init(); rational_tests(); ring_tests();
 for(i=0;i<3;i++) {
  uint64_t va=0x40000000ULL+(uint64_t)i*4096;
  phys[i]=pmm_alloc_frame(); require(phys[i]!=0,"PMM frame");
  require(mmu_map(boot_pml4,va,phys[i],PAGE_WRITE),"map SLS");
  if(i<2) {
   tasks[i]=(omega_task_t *)va;
   if(!sls_register(va)) init_task(tasks[i],i+1);
   require(omega_verify_contract(tasks[i]),"recovered task contract");
   uart_puts("TASK recovered/start epoch="); uart_put_dec(tasks[i]->epoch); uart_puts("\n");
  } else {
   if(sls_register(va)) {
    for(unsigned j=0;j<4096;j++) require(probe[j]==(uint8_t)(j^0xa5),"recovered probe bytes");
    uart_puts("TEST persistent probe recovered after reboot PASS\n");
   }
   for(unsigned j=0;j<4096;j++) probe[j]=(uint8_t)(j^0xa5);
  }
 }
 if(crash_test) {
  for(i=0;i<2;i++) { tasks[i]->epoch++; require(rational_make(1,2,&tasks[i]->state[0]),"crash-test state");
   tasks[i]->proof_hash=omega_contract_token(tasks[i]); }
 }
 require(mmu_sls_commit(boot_pml4)==3,"initial disk commit");
 require(mmu_sls_commit(boot_pml4)==0,"clean pages must not commit again");
 memset(probe,0,4096); require(sls_restore((uint64_t)probe),"disk readback");
 for(i=0;i<4096;i++) require(probe[i]==(uint8_t)(i^0xa5),"disk DMA bytes");
 uart_puts("TEST VirtIO disk DMA readback PASS\n");
 start=rdtsc(); smp_start(&channel,tasks[1]);
 for(uint64_t step=0;step<SMP_MESSAGES;step++) {
  require(rational_make((int64_t)(step%4),8,&tasks[0]->state[0]),"producer rational");
  tasks[0]->epoch++; tasks[0]->proof_hash=omega_contract_token(tasks[0]);
  require(omega_verify_contract(tasks[0]),"producer contract");
  while(!ring_push(&channel,step)) {
   require(rdtsc()-start<30000000000ULL,"producer timeout"); cpu_pause();
  }
 }
 smp_wait(); cycles=rdtsc()-start;
 /* AP is quiescent. All subsequent MMU edits are BSP-only; no missing remote
  * TLB invalidation is hidden behind a claim of general SMP scheduling. */
 require(omega_verify_contract(tasks[1]),"consumer final contract");
 dirty=mmu_sls_commit(boot_pml4); require(dirty>=2,"AP/BSP dirty pages");
 /* Exercise a sparse logical address at the end of the first GiB. */
 { uint64_t frame=pmm_alloc_frame(); uint64_t va=0x7ffff000;
  require(frame&&mmu_map(boot_pml4,va,frame,PAGE_WRITE),"GiB sparse mapping");
  (void)sls_register(va); memcpy((void *)va,"OMEGA-GIB-SPARSE",16);
  require(mmu_sls_commit(boot_pml4)!=0,"GiB tree commit");
 }
 isolation_test((uint64_t)tasks[0]);
 require(sls_restore((uint64_t)tasks[0])&&omega_verify_contract(tasks[0]),"post-isolation restore");
 mmu_harden();
 (void)net_init();
 if(!runtime_run()){uart_puts("REJECTED V3: mutation quarantined; kernel context retained; HLT\n");for(;;)__asm__ volatile("cli; hlt");}
 if(stress){
  for(unsigned round=0;round<160;round++){
   for(i=0;i<2;i++){tasks[i]->epoch++;tasks[i]->proof_hash=omega_contract_token(tasks[i]);}
   require(mmu_sls_commit(boot_pml4)==2,"GC stress transaction");
  }
  for(i=0;i<2;i++)require(sls_restore((uint64_t)tasks[i])&&omega_verify_contract(tasks[i]),"GC stress restore");
  uart_puts("TEST GC 160 durable multipage commits on bounded disk PASS\n");
 }
 if(cluster_config.enabled)consensus_run();
 net_test();
 uart_puts("CPU TSC cycles: "); uart_put_dec(cycles);
 uart_puts("\nSTABLE V4: native/incubator/GC/tunnel complete; HLT\n");
 for(;;) __asm__ volatile("cli; hlt");
}
