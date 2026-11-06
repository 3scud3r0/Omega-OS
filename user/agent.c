/* Native Ring 3 program. Compiled separately; no kernel symbols or headers. */
typedef unsigned long long u64;
static u64 ticks(void) { unsigned l,h; __asm__ volatile("lfence; rdtsc":"=a"(l),"=d"(h)::"memory"); return ((u64)h<<32)|l; }
static u64 call(u64 nr,u64 arg) {
 u64 result;
 __asm__ volatile("syscall":"=a"(result):"a"(nr),"D"(arg):"rcx","r11","memory"); return result;
}
void _start(void) {
 volatile u64 *data=(volatile u64 *)0x80002000ULL;
 u64 role=data[0],i;
 if(call(0,0)!=role+1) (void)call(3,1);
 if(data[22]){if(data[24])*(volatile unsigned char *)0x83000000ULL=0xf4;u64 ok=((u64 (*)(void *))0x83000000ULL)((void *)(data+6));if(!ok)(void)call(3,1);(void)call(data[23],0);}
 if(role==0) {
  { u64 mac=call(7,0);
   if(mac) { volatile unsigned char *packet=(volatile unsigned char *)0x80003000ULL;
    for(i=0;i<64;i++) packet[i]=0;
    for(i=0;i<6;i++) { packet[i]=255; packet[6+i]=(unsigned char)(mac>>(i*8)); }
    packet[12]=0x88; packet[13]=0xb5;
    { const char text[]="OMEGA-USER-DMA"; for(i=0;i<sizeof(text);i++) packet[14+i]=(unsigned char)text[i]; }
    if(call(4,64)) (void)call(3,1);
   }
  }
  { const char source[]="tensor result = (7+5)*2; mostrar result;";
   volatile unsigned char *p=(volatile unsigned char *)0x80003000ULL;
   u64 length=data[21];
   if(!length){length=sizeof source;for(i=0;i<length;i++)p[i]=(unsigned char)source[i];}
   if(call(9,length)) (void)call(3,1);
  }
  for(i=0;i<256;i++) while(call(1,i)!=0) { __asm__ volatile("pause"); }
  for(;;) { data[1]++; __asm__ volatile("pause"); }
 }
 if(role==1) {
  for(i=0;i<256;i++) {
   u64 value; do { value=call(2,0); } while(value==~0ULL);
   if(value!=i) (void)call(3,1);
  }
  { u64 begin=ticks(); for(i=0;i<1024;i++) (void)call(0,0); data[2]=(ticks()-begin)/1024; }
  data[1]=256; (void)call(3,0);
 }
 if(role==4) { __asm__ volatile("movabsq $0xffff800000000000,%%rsp; xorq %%rax,%%rax; syscall; ud2":::"rax","rcx","r11","memory"); }
 if(role==3) { ((void (*)(void))0x80001000ULL)(); (void)call(3,1); }
 if(role==2) { data[1]=*(volatile u64 *)0x90000000ULL; (void)call(3,1); }
 for(;;) __asm__ volatile("pause");
}
