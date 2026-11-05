#include "smp.h"
#include "mmu.h"
#include "idt.h"
#include "uart.h"
uint8_t ap_stack[16384] ALIGN_4K __attribute__((used,externally_visible));
static volatile uint32_t *lapic;
static uint32_t ap_id;
static uint32_t online ALIGN_64,done ALIGN_64;
static omega_ring_t *ipc;
static omega_task_t *consumer_task;
extern const uint8_t ap_trampoline[],ap_trampoline_end[];
void ap_entry(void) __attribute__((used,externally_visible,noreturn));
__asm__(
".section .text.ap,\"ax\"\n"
".balign 16\n"
".global ap_trampoline,ap_trampoline_end\n"
"ap_trampoline:\n"
".code16\n"
"cli\n"
"cld\n"
"xorw %ax,%ax\n"
"movw %ax,%ds\n"
"movw %ax,%es\n"
"movw %ax,%ss\n"
"lgdtl (0x8000+ap_gdtr-ap_trampoline)\n"
"movl %cr0,%eax\n"
"orl $1,%eax\n"
"movl %eax,%cr0\n"
"ljmpl $8,$(0x8000+ap_protected-ap_trampoline)\n"
".code32\n"
"ap_protected:\n"
"movw $16,%ax\n"
"movw %ax,%ds\n"
"movw %ax,%es\n"
"movw %ax,%ss\n"
"movl %cr4,%eax\n"
"orl $0x20,%eax\n"
"movl %eax,%cr4\n"
"movl $boot_pml4,%eax\n"
"movl %eax,%cr3\n"
"movl $0xc0000080,%ecx\n"
"rdmsr\n"
"orl $0x100,%eax\n"
"wrmsr\n"
"movl %cr0,%eax\n"
"orl $0x80010000,%eax\n"
"movl %eax,%cr0\n"
"ljmpl $24,$(0x8000+ap_long-ap_trampoline)\n"
".code64\n"
"ap_long:\n"
"movabsq $(ap_stack+16384),%rsp\n"
"xorq %rbp,%rbp\n"
"movabsq $ap_entry,%rax\n"
"call *%rax\n"
"1: cli\n"
"hlt\n"
"jmp 1b\n"
".balign 8\n"
"ap_gdt:\n"
".quad 0,0x00cf9a000000ffff,0x00cf92000000ffff,0x00af9a000000ffff\n"
"ap_gdtr:\n"
".word 31\n"
".long (0x8000+ap_gdt-ap_trampoline)\n"
"ap_trampoline_end:\n"
".text\n"
);

static uint32_t read32(const uint8_t *p) { uint32_t v; memcpy(&v,p,4); return v; }
static uint64_t read64(const uint8_t *p) { uint64_t v; memcpy(&v,p,8); return v; }
static bool signature(const uint8_t *p,const char *s,unsigned n) {
 unsigned i; for(i=0;i<n;i++) if(p[i]!=(uint8_t)s[i]) return false; return true;
}
static bool checksum(const uint8_t *p,unsigned n) {
 uint8_t sum=0; unsigned i; for(i=0;i<n;i++) sum=(uint8_t)(sum+p[i]); return sum==0;
}
static const uint8_t *table(uint64_t addr) {
 const uint8_t *p; uint32_t len;
 if(!addr||!mmu_map_device(addr,36)) panic("ACPI address unsupported");
 p=(const uint8_t *)addr; len=read32(p+4);
 if(len<36||len>1048576||!mmu_map_device(addr,len)||!checksum(p,len)) panic("ACPI table checksum/length");
 return p;
}
static const uint8_t *find_rsdp(uint64_t start,uint64_t end) {
 uint64_t a;
 for(a=start;a+20<=end;a+=16) {
  const uint8_t *p=(const uint8_t *)a;
  if(signature(p,"RSD PTR ",8)&&checksum(p,20)) return p;
 }
 return 0;
}
static uint64_t discover(void) {
 const uint8_t *rsdp,*root,*madt=0; uint16_t ebda; uint64_t lapic_addr; unsigned stride,i,len;
 uint32_t bsp; bool found=false;
 /* Volatile avoids treating BIOS low memory as a C null-object access. */
 __asm__ volatile("movw 0x40e,%0":"=r"(ebda));
 rsdp=ebda>=0x8000&&ebda<=0x9fc0?find_rsdp((uint64_t)ebda<<4,((uint64_t)ebda<<4)+1024):0;
 if(!rsdp) rsdp=find_rsdp(0xe0000,0x100000);
 if(!rsdp) panic("ACPI RSDP absent");
 if(rsdp[15]>=2) {
  uint32_t n=read32(rsdp+20);
  if(n<36||n>4096||!checksum(rsdp,n)) panic("ACPI extended RSDP");
  root=table(read64(rsdp+24)); stride=8;
  if(!signature(root,"XSDT",4)) panic("ACPI XSDT signature");
 } else { root=table(read32(rsdp+16)); stride=4; if(!signature(root,"RSDT",4)) panic("ACPI RSDT signature"); }
 len=read32(root+4);
 if((len-36)%stride) panic("ACPI root entries");
 for(i=36;i<len;i+=stride) {
  const uint8_t *t=table(stride==8?read64(root+i):read32(root+i));
  if(signature(t,"APIC",4)) madt=t;
 }
 if(!madt||read32(madt+4)<44) panic("ACPI MADT absent");
 lapic_addr=read32(madt+36); len=read32(madt+4);
 for(i=44;i<len;) {
  unsigned n;
  if(len-i<2) panic("MADT truncated entry");
  n=madt[i+1]; if(n<2||n>len-i) panic("MADT entry length");
  if(madt[i]==5) { if(n<12) panic("MADT LAPIC override"); lapic_addr=read64(madt+i+4); }
  i+=n;
 }
 if((lapic_addr&4095)||!lapic_addr||!mmu_map_device(lapic_addr,4096)) panic("LAPIC mapping");
 lapic=(volatile uint32_t *)lapic_addr; bsp=lapic[0x20/4]>>24;
 for(i=44;i<len;i+=madt[i+1]) {
  uint32_t id=~0U,flags=0;
  if(madt[i]==0&&madt[i+1]>=8) { id=madt[i+3]; flags=read32(madt+i+4); }
  if(madt[i]==9&&madt[i+1]>=16) { id=read32(madt+i+4); flags=read32(madt+i+8); }
  if((flags&1)&&id!=bsp&&id<256&&!found) { ap_id=id; found=true; }
 }
 if(!found) panic("MADT no second enabled xAPIC CPU");
 uart_puts("ACPI MADT BSP="); uart_put_dec(bsp); uart_puts(" AP="); uart_put_dec(ap_id); uart_puts("\n");
 return lapic_addr;
}
/* PIT channel 2 gives real elapsed time, independent of uncalibrated TSC rate. */
static void delay_us(unsigned us) {
 uint8_t old=inb(0x61); uint16_t ticks=(uint16_t)((us*1193182ULL+999999)/1000000);
 uint64_t start=rdtsc();
 outb(0x61,(uint8_t)(old&~3U)); outb(0x43,0xb0);
 outb(0x42,(uint8_t)ticks); outb(0x42,(uint8_t)(ticks>>8)); outb(0x61,(uint8_t)((old&~2U)|1U));
 while(!(inb(0x61)&0x20)) { if(rdtsc()-start>10000000000ULL) panic("PIT timeout"); cpu_pause(); }
 outb(0x61,old);
}
static void send_ipi(uint32_t value) {
 uint64_t start=rdtsc();
 while(lapic[0x300/4]&0x1000) { if(rdtsc()-start>1000000000ULL) panic("LAPIC busy"); cpu_pause(); }
 lapic[0x310/4]=ap_id<<24; lapic[0x300/4]=value;
 while(lapic[0x300/4]&0x1000) { if(rdtsc()-start>1000000000ULL) panic("LAPIC delivery timeout"); cpu_pause(); }
}
void smp_start(omega_ring_t *ring,omega_task_t *consumer) {
 uint32_t lo,hi; uint64_t base,start; size_t length=(size_t)(ap_trampoline_end-ap_trampoline);
 __asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(0x1b));
 if(lo&(1U<<10)) panic("x2APIC mode unsupported; use xAPIC");
 base=discover();
 if((base&~4095ULL)!=(((uint64_t)hi<<32|lo)&0xfffff000ULL)) panic("LAPIC MSR/MADT mismatch");
 lo|=1U<<11; __asm__ volatile("wrmsr"::"a"(lo),"d"(hi),"c"(0x1b));
 lapic[0xf0/4]=0x1ff;
 if(length>4096) panic("AP trampoline too large");
 memcpy((void *)0x8000,ap_trampoline,length); ipc=ring; consumer_task=consumer; mfence();
 send_ipi(0xc500); delay_us(10000); send_ipi(0x8500); delay_us(200);
 send_ipi(0x4608); delay_us(200); send_ipi(0x4608);
 start=rdtsc();
 while(!__atomic_load_n(&online,__ATOMIC_ACQUIRE)) {
  if(rdtsc()-start>10000000000ULL) panic("AP startup timeout");
  cpu_pause();
 }
 uart_puts("SMP Core1 online via INIT-SIPI-SIPI; consumer running\n");
}
void ap_entry(void) {
 uint64_t i,value,start=rdtsc();
 idt_init_cpu(1);
 if((lapic[0x20/4]>>24)!=ap_id) panic("AP identity mismatch");
 lapic[0xf0/4]=0x1ff; __atomic_store_n(&online,1,__ATOMIC_RELEASE);
 for(i=0;i<SMP_MESSAGES;i++) {
  while(!ring_pop(ipc,&value)) {
   if(rdtsc()-start>30000000000ULL) panic("AP consumer timeout");
   cpu_pause();
  }
  if(value!=i) panic("SMP FIFO corruption");
  consumer_task->state[0].num=(int64_t)(value%4); consumer_task->state[0].den=8;
  if(!rational_make(consumer_task->state[0].num,8,&consumer_task->state[0])) panic("AP rational state");
  consumer_task->epoch++; consumer_task->proof_hash=omega_contract_token(consumer_task);
  if(!omega_verify_contract(consumer_task)) panic("AP task contract");
 }
 __atomic_store_n(&done,1,__ATOMIC_RELEASE);
 for(;;) __asm__ volatile("cli; hlt");
}
void smp_wait(void) {
 uint64_t start=rdtsc();
 while(!__atomic_load_n(&done,__ATOMIC_ACQUIRE)) { if(rdtsc()-start>30000000000ULL) panic("AP completion timeout"); cpu_pause(); }
 uart_puts("TEST SMP SPSC 65536 ordered messages on distinct APIC IDs PASS\n");
}

void smp_timer_start(void) {
 uint32_t elapsed;
 lapic[0x3e0/4]=3; lapic[0x320/4]=0x10030; lapic[0x380/4]=0xffffffffU;
 delay_us(10000); elapsed=0xffffffffU-lapic[0x390/4];
 if(elapsed<10) panic("LAPIC timer calibration");
 lapic[0x320/4]=0x20030; lapic[0x380/4]=elapsed/10;
}
void smp_timer_stop(void) { lapic[0x320/4]=0x10030; lapic[0x380/4]=0; }
void smp_timer_eoi(void) { lapic[0xb0/4]=0; }
