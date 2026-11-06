"""Provision three pinned identities offline. Never include these initrds in bundles."""
import argparse
import hashlib
import ipaddress
import os
from pathlib import Path
import struct
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

def provision(directory):
    directory.mkdir(mode=0o700,parents=True,exist_ok=True)
    private=[(os.urandom(32),os.urandom(32)) for _ in range(3)]
    members=b''.join(struct.pack('<IIH6x32s32s',i+1,int(ipaddress.IPv4Address(f'10.{21+i}.0.2')),40000,
        X25519PrivateKey.from_private_bytes(x).public_key().public_bytes_raw(),
        Ed25519PrivateKey.from_private_bytes(ed).public_key().public_bytes_raw()) for i,(x,ed) in enumerate(private))
    identity=hashlib.blake2b(members,digest_size=32).digest();paths=[]
    for i,(x,ed) in enumerate(private):
        header=struct.pack('<8sIIII8x32s32s32s',b'OMCLUST4',400,i,0xffffff00,int(ipaddress.IPv4Address(f'10.{21+i}.0.1')),x,ed,identity)
        body=header+members;assert len(body)==368
        path=directory/f'node-{i+1}.conf';fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
        with os.fdopen(fd,'wb') as stream:stream.write(body+hashlib.blake2b(body,digest_size=32).digest())
        paths.append(path)
    return paths
if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--output-dir',type=Path,required=True);args=parser.parse_args()
    provision(args.output_dir);print('Three private boot modules saved with mode 0600; no private keys printed.')
