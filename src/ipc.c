#include "ipc.h"
typedef char head_tail_separate[(__builtin_offsetof(omega_ring_t,tail)==64)?1:-1];
void ring_init(omega_ring_t *r) { r->head=0; r->tail=0; }
bool ring_push(omega_ring_t *r, uint64_t v) {
 uint64_t h=__atomic_load_n(&r->head,__ATOMIC_RELAXED);
 uint64_t t=__atomic_load_n(&r->tail,__ATOMIC_ACQUIRE);
 if(h-t==RING_CAPACITY) return false;
 r->slots[h&(RING_CAPACITY-1)]=v; __atomic_store_n(&r->head,h+1,__ATOMIC_RELEASE); return true;
}
bool ring_pop(omega_ring_t *r, uint64_t *v) {
 uint64_t t=__atomic_load_n(&r->tail,__ATOMIC_RELAXED);
 uint64_t h=__atomic_load_n(&r->head,__ATOMIC_ACQUIRE);
 if(t==h) return false;
 *v=r->slots[t&(RING_CAPACITY-1)]; __atomic_store_n(&r->tail,t+1,__ATOMIC_RELEASE); return true;
}
