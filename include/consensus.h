#ifndef OMEGA_CONSENSUS_H
#define OMEGA_CONSENSUS_H
#include "types.h"
#define CONSENSUS_NODES 3U
#define LINEAGE_VA 0x40006000ULL
/* Signed majority with a fixed, honest leader. This is NOT general BFT:
 * n=3,q=2 cannot tolerate one equivocating Byzantine identity. */
typedef struct {uint32_t id,ip;uint16_t port;uint8_t reserved[6],xkey[32],edkey[32];} cluster_member_t;
typedef struct {bool enabled;unsigned self;uint64_t burst_ticks;uint8_t id[32],xsecret[32],edsecret[64];cluster_member_t member[3];} cluster_config_t;
extern cluster_config_t cluster_config;
typedef struct {uint64_t magic,epoch;uint8_t previous[32],hash[32];uint64_t voters;uint8_t signatures[3][64],payload[2816],padding[1000];} lineage_record_t;
typedef char lineage_page_size[(sizeof(lineage_record_t)==4096)?1:-1];
void consensus_provision(uint8_t *module,size_t n);
bool consensus_gate(uint64_t va,const void *data);
bool consensus_certificate(const lineage_record_t *r);
void consensus_run(void);
#endif
