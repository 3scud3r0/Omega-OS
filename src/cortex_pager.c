#include "cortex.h"
#include "mmu.h"
#include "uart.h"
#define ADDRESS(id) (0x40000000ULL + (id) * 4096)
typedef struct {
    uint64_t id, physical, age;
    unsigned pins;
    bool used, writable;
} block_t;
static block_t blocks[CORTEX_CACHE_FRAMES];
static cortex_pager_stats_t stats;
static uint64_t root, clock_age;
static uint8_t checkpoint_page[4096] ALIGN_4K, output_page[4096] ALIGN_4K;
static bool output_pending;
void cortex_pager_output(const void *data) {
    memcpy(output_page, data, 4096);
    output_pending = true;
}
static bool dirty(unsigned i) {
    uint64_t *p = mmu_leaf((uint64_t *)root, CORTEX_CACHE_VA + 4096 * i);
    return p && (*p & PAGE_DIRTY);
}
static bool stage(unsigned i) {
    if (!blocks[i].used || !dirty(i))
        return true;
    if (!blocks[i].writable || !sls_stage_page(ADDRESS(blocks[i].id), (void *)blocks[i].physical))
        return false;
    stats.writes++;
    return true;
}
static void clean(unsigned i) {
    uint64_t *p = mmu_leaf((uint64_t *)root, CORTEX_CACHE_VA + 4096 * i);
    if (p)
        *p &= ~PAGE_DIRTY;
    invlpg(CORTEX_CACHE_VA + 4096 * i);
}
void cortex_pager_begin(uint64_t r) {
    memset(blocks, 0, sizeof blocks);
    memset(&stats, 0, sizeof stats);
    clock_age = 0;
    root = r;
    output_pending = false;
}
uint64_t cortex_pager_map(uint64_t id, bool write) {
    unsigned slot = CORTEX_CACHE_FRAMES;
    if (id >= 16777216ULL || id < CORTEX_META ||
        (!write && id < CORTEX_MODEL && id != CORTEX_META && id != CORTEX_CHECKPOINT) ||
        (write && id < CORTEX_KV))
        return ~0ULL;
    for (unsigned i = 0; i < CORTEX_CACHE_FRAMES; i++)
        if (blocks[i].used && blocks[i].id == id) {
            slot = i;
            stats.hits++;
            break;
        }
    if (slot == CORTEX_CACHE_FRAMES) {
        for (unsigned i = 0; i < CORTEX_CACHE_FRAMES; i++)
            if (!blocks[i].pins && (!blocks[i].used || slot == CORTEX_CACHE_FRAMES ||
                                    blocks[i].age < blocks[slot].age)) {
                slot = i;
                if (!blocks[i].used)
                    break;
            }
        if (slot == CORTEX_CACHE_FRAMES)
            return ~0ULL;
        block_t *b = &blocks[slot];
        if (b->used) {
            if (dirty(slot)) {
                sls_begin_pages(1);
                if (!stage(slot) || !sls_commit())
                    panic("cortex eviction I/O");
            }
            stats.evictions++;
        } else if (!b->physical) {
            b->physical = pmm_alloc_frame();
            if (!b->physical)
                return ~0ULL;
            stats.resident++;
            if (stats.resident > stats.peak)
                stats.peak = stats.resident;
        }
        /* Revoke the old capability before reusing its frame. */
        uint64_t *leaf = mmu_leaf((uint64_t *)root, CORTEX_CACHE_VA + slot * 4096);
        if (leaf)
            *leaf = 0;
        invlpg(CORTEX_CACHE_VA + slot * 4096);
        bool present = sls_read_page(ADDRESS(id), (void *)b->physical);
        stats.reads++;
        if (!present) {
            if (!write) {
                b->used = false;
                return ~0ULL;
            }
            memset((void *)b->physical, 0, 4096);
        }
        b->id = id;
        b->writable = write;
        b->used = true;
        stats.logical_pages++;
    }
    block_t *b = &blocks[slot];
    b->age = ++clock_age;
    b->pins++;
    if (write)
        b->writable = true;
    uint64_t *leaf = mmu_leaf((uint64_t *)root, CORTEX_CACHE_VA + slot * 4096);
    if (leaf && (*leaf & PAGE_PRESENT)) {
        if (b->writable && !(*leaf & PAGE_WRITE)) {
            *leaf |= PAGE_WRITE;
            invlpg(CORTEX_CACHE_VA + slot * 4096);
        }
    } else if (!mmu_map((uint64_t *)root, CORTEX_CACHE_VA + slot * 4096, b->physical,
                        PAGE_USER | PAGE_NX | (b->writable ? PAGE_WRITE : 0)))
        panic("cortex pager map");
    return CORTEX_CACHE_VA + slot * 4096;
}
bool cortex_pager_release(uint64_t va) {
    if (va < CORTEX_CACHE_VA || va >= CORTEX_CACHE_VA + 4096 * CORTEX_CACHE_FRAMES || (va & 4095))
        return false;
    unsigned i = (unsigned)((va - CORTEX_CACHE_VA) / 4096);
    if (!blocks[i].pins)
        return false;
    blocks[i].pins--;
    return true;
}
bool cortex_checkpoint_valid(const cortex_checkpoint_t *c) {
    if (c->magic != CORTEX_MAGIC || !c->generation || !c->dim || c->dim > 8192 || !c->hidden ||
        c->hidden > 32768 || !c->layers || c->layers > 128 || !c->heads || c->heads > 128 ||
        !c->kv_heads || c->heads % c->kv_heads || c->dim % c->heads || c->vocab < 2 ||
        c->vocab > 262144 || !c->context || c->context > 1048576 || c->position > c->context ||
        c->token >= c->vocab || c->kv_pages > 0x800000ULL || !c->model_bytes)
        return false;
    uint64_t dim = (uint64_t)c->dim * c->kv_heads / c->heads;
    uint64_t pages = ((c->context * dim * 4 + 4095) / 4096) * c->layers;
    return c->position && c->generation <= c->position && c->accepted == c->position &&
           c->rejected <= (uint64_t)c->vocab * c->position && !(c->flags & ~1U) &&
           c->kv_pages == pages && c->logical_bytes == pages * 4096 &&
           c->model_bytes < (CORTEX_KV - CORTEX_MODEL) * 4096;
}
bool cortex_pager_commit(const cortex_checkpoint_t *c) {
    if (!cortex_checkpoint_valid(c))
        return false;
    sls_begin_pages(CORTEX_CACHE_FRAMES + 2);
    for (unsigned i = 0; i < CORTEX_CACHE_FRAMES; i++)
        if (!stage(i))
            return false;
    memset(checkpoint_page, 0, 4096);
    memcpy(checkpoint_page, c, sizeof *c);
    if (output_pending && !sls_stage_page(ADDRESS(CORTEX_CHECKPOINT + 1), output_page))
        return false;
    if (!sls_stage_page(ADDRESS(CORTEX_CHECKPOINT), checkpoint_page) || !sls_commit())
        return false;
    output_pending = false;
    for (unsigned i = 0; i < CORTEX_CACHE_FRAMES; i++)
        clean(i);
    uart_puts("CORTEX SLS atomic checkpoint position=");
    uart_put_dec(c->position);
    uart_puts(" resident frames=");
    uart_put_dec(stats.peak);
    uart_puts("\n");
    return true;
}
const cortex_pager_stats_t *cortex_pager_stats(void) { return &stats; }
void cortex_pager_end(void) {
    for (unsigned i = 0; i < CORTEX_CACHE_FRAMES; i++)
        if (blocks[i].physical) {
            uint64_t *p = mmu_leaf((uint64_t *)root, CORTEX_CACHE_VA + 4096 * i);
            if (p)
                *p = 0;
            invlpg(CORTEX_CACHE_VA + 4096 * i);
            if (!pmm_free_frame(blocks[i].physical))
                panic("pager frame release");
        }
    memset(blocks, 0, sizeof blocks);
}
