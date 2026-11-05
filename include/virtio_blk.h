#ifndef OMEGA_VIRTIO_BLK_H
#define OMEGA_VIRTIO_BLK_H
#include "types.h"
#define VIRTIO_BLK_T_OUT 1U
#define VIRTQUEUE_MAX 1024U
typedef struct { uint64_t addr; uint32_t len; uint16_t flags,next; } vring_desc;
typedef struct { uint16_t flags,idx,ring[VIRTQUEUE_MAX]; } vring_avail;
typedef struct { uint32_t id,len; } vring_used_elem;
typedef struct { uint16_t flags,idx; vring_used_elem ring[VIRTQUEUE_MAX]; } vring_used;
void virtio_blk_init(void);
bool virtio_blk_submit(uint64_t sector,const void *page,bool write);
bool virtio_blk_poll(bool *ok);
bool virtio_blk_write_page(uint64_t sector,const void *page);
bool virtio_blk_read_page(uint64_t sector,void *page);
bool virtio_blk_flush(void);
uint64_t virtio_blk_capacity(void);
bool virtio_blk_sector(uint64_t sector,void *data,bool write);
#endif
