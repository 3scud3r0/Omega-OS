#include "consensus.h"
#include "consensus_vectors.h"
int main(void){
 lineage_record_t r;memcpy(cluster_config.id,cluster_id,32);for(unsigned i=0;i<3;i++)memcpy(cluster_config.member[i].edkey,public_keys[i],32);
 memcpy(&r,valid_record,4096);if(!consensus_certificate(&r))return 1;
 r.payload[20]^=1;if(consensus_certificate(&r))return 2;
 memcpy(&r,valid_record,4096);r.voters=1;if(consensus_certificate(&r))return 3;
 memcpy(&r,valid_record,4096);r.voters=7;memcpy(r.signatures[2],r.signatures[1],64);if(consensus_certificate(&r))return 4;
 memcpy(&r,valid_record,4096);r.previous[0]^=1;if(consensus_certificate(&r))return 5;
 memcpy(&r,valid_record,4096);r.padding[999]=1;if(consensus_certificate(&r))return 6;
 memcpy(&r,invalid_semantics,4096);if(consensus_certificate(&r))return 7;
 memcpy(&r,valid_record,4096);cluster_config.id[0]^=1;if(consensus_certificate(&r))return 8;
 return 0;
}
