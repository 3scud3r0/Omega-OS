/* Hosted test only: kernel objects themselves have no libc dependencies. */
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include "ipc.h"
#define MESSAGES 1000000ULL
static omega_ring_t ring;
static void *produce(void *unused) {
 uint64_t i; (void)unused;
 for(i=0;i<MESSAGES;i++) while(!ring_push(&ring,i)) sched_yield();
 return 0;
}
static void *consume(void *unused) {
 uint64_t i,v; (void)unused;
 for(i=0;i<MESSAGES;i++) {
  while(!ring_pop(&ring,&v)) sched_yield();
  if(v!=i) { fputs("FIFO corruption\n",stderr); exit(1); }
 }
 return 0;
}
int main(void) {
 pthread_t producer,consumer;
 ring_init(&ring);
 if(pthread_create(&producer,0,produce,0)||pthread_create(&consumer,0,consume,0)) return 1;
 if(pthread_join(producer,0)||pthread_join(consumer,0)) return 1;
 puts("PASS: 1000000 SPSC messages between two host threads"); return 0;
}
