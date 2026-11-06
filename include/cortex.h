#ifndef OMEGA_CORTEX_H
#define OMEGA_CORTEX_H
#include "types.h"
#include "rational.h"
#define CORTEX_META 0x1000ULL
#define CORTEX_CHECKPOINT 0x1001ULL
#define CORTEX_MODEL 0x2000ULL
#define CORTEX_KV 0x800000ULL
#define CORTEX_CACHE_VA 0xa0000000ULL
#define CORTEX_CACHE_FRAMES 64U
#define CORTEX_MAGIC 0x315854524f434d4fULL
#define CORTEX_MANIFEST_OFFSET 2320U
typedef struct {
    uint64_t magic, generation, position, token, model_bytes, kv_pages, logical_bytes;
    uint32_t dim, hidden, layers, heads, kv_heads, vocab, context, flags;
    uint8_t model_hash[32], kv_hash[32], logit_hash[32], jit_hash[32];
    uint64_t accepted, rejected;
    uint8_t reserved[24];
} cortex_checkpoint_t;
typedef char cortex_checkpoint_size[(sizeof(cortex_checkpoint_t) == 256) ? 1 : -1];
typedef struct {
    uint64_t resident, peak, reads, writes, hits, evictions, logical_pages;
} cortex_pager_stats_t;
void cortex_pager_begin(uint64_t root);
uint64_t cortex_pager_map(uint64_t id, bool write);
bool cortex_pager_release(uint64_t va);
bool cortex_pager_commit(const cortex_checkpoint_t *checkpoint);
void cortex_pager_output(const void *data);
void cortex_pager_end(void);
const cortex_pager_stats_t *cortex_pager_stats(void);
bool cortex_checkpoint_valid(const cortex_checkpoint_t *c);
/* Public numerical interfaces; memory B is row-major N x K. */
bool cortex_gemm_i8(const signed char *a, const signed char *b, int64_t *c, uint32_t m, uint32_t n,
                    uint32_t k, bool vector);
typedef struct {
    uint32_t token, reserved;
    omega_rational_t value;
} cortex_contract_t;
unsigned cortex_symbolic_prune(float *logits, unsigned vocabulary,
                               const cortex_contract_t *contracts, unsigned count);
float cortex_fp16(uint16_t value);
float cortex_dequant_value(unsigned type, const uint8_t *block, unsigned index);
#endif
