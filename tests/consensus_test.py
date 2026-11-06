"""Independent RFC8032 signing of the kernel's exact application certificate."""
import hashlib
from pathlib import Path
import struct
import subprocess
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
keys=[Ed25519PrivateKey.generate() for _ in range(3)]
cluster=hashlib.blake2b(b'host-only-consensus-vector',digest_size=32).digest()
def record(bad=False):
    organisms=b''.join(struct.pack('<4Q2q96s',i+1,1,42,512,25 if bad else 24,1,b'tensor result = 24; mostrar result;') for i in range(16))
    payload=(b'OMEGA-LINEAGE-V4'+organisms).ljust(2816,b'\0')
    digest=hashlib.blake2b(payload,digest_size=32).digest();epoch=struct.pack('<Q',1);previous=bytes(32)
    statement=b'OMEGA-QUORUM-V4\0'+cluster+epoch+previous+digest
    signatures=keys[0].sign(statement)+keys[1].sign(statement)+bytes(64)
    return b'LINEAGE4'+epoch+previous+digest+struct.pack('<Q',3)+signatures+payload+bytes(1000)
def array(name,data):return 'static const uint8_t '+name+'['+str(len(data))+']={'+','.join(map(str,data))+'};\n'
header=array('cluster_id',cluster)+array('valid_record',record())+array('invalid_semantics',record(True))
header+='static const uint8_t public_keys[3][32]={'+','.join('{'+','.join(map(str,k.public_key().public_bytes_raw()))+'}' for k in keys)+'};\n'
Path('build/consensus_vectors.h').write_text(header)
subprocess.run(['gcc','-std=c99','-O2','-Wall','-Wextra','-Werror','-pedantic','-ffunction-sections','-fdata-sections','-Iinclude','-Iinclude/crypto','-Ibuild','tests/consensus_harness.c','src/consensus.c','src/crypto/monocypher.c','src/crypto/monocypher-ed25519.c','-Wl,--gc-sections','-o','build/consensus_test'],check=True)
subprocess.run(['build/consensus_test'],check=True)
print('PASS signed-majority certificates: public-key verification, quorum, identity, domain, content and semantic rejection')
