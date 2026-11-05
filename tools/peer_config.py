"""Offline X25519 peer provisioning. Private material is never printed."""
import argparse
import hashlib
import ipaddress
import os
from pathlib import Path
import struct
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey

def create(path,data,mode):
    fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL,mode)
    with os.fdopen(fd,'wb') as stream:stream.write(data)

parser=argparse.ArgumentParser()
sub=parser.add_subparsers(dest='command',required=True)
k=sub.add_parser('keygen');k.add_argument('--private',type=Path,required=True);k.add_argument('--public',type=Path,required=True)
c=sub.add_parser('config')
for option in ['private-file','peer-public-file','output']:c.add_argument('--'+option,type=Path,required=True)
for option in ['local-ip','peer-ip','gateway']:c.add_argument('--'+option,required=True)
c.add_argument('--prefix',type=int,default=24);c.add_argument('--local-port',type=int,default=40000);c.add_argument('--peer-port',type=int,default=40000)
a=parser.parse_args()
if a.command=='keygen':
    if a.private.exists() or a.public.exists():raise SystemExit('Refusing to overwrite an existing identity')
    private=os.urandom(32);public=X25519PrivateKey.from_private_bytes(private).public_key().public_bytes_raw()
    create(a.private,private,0o600);create(a.public,public,0o644)
else:
    secret=a.private_file.read_bytes();public=a.peer_public_file.read_bytes()
    if len(secret)!=32 or len(public)!=32:raise SystemExit('X25519 raw key files must contain exactly 32 bytes')
    if not 0<=a.prefix<=32 or not 1<=a.local_port<=65535 or not 1<=a.peer_port<=65535:raise SystemExit('Invalid prefix or port')
    local=ipaddress.IPv4Address(a.local_ip);peer=ipaddress.IPv4Address(a.peer_ip);gateway=ipaddress.IPv4Address(a.gateway)
    network=ipaddress.IPv4Network(f'{local}/{a.prefix}',strict=False)
    if local==peer or local.is_multicast or peer.is_multicast or local.is_unspecified or peer.is_unspecified:raise SystemExit('Unicast distinct peer endpoints required')
    if peer not in network and (gateway not in network or gateway.is_unspecified):raise SystemExit('Off-subnet peer requires a reachable gateway')
    body=struct.pack('<8sI4I2H32s32s',b'OMNETV3\0',128,int(local),int(peer),int(gateway),int(network.netmask),a.local_port,a.peer_port,secret,public)
    create(a.output,body+hashlib.blake2b(body,digest_size=32).digest(),0o600)
print('Provisioning files saved; private material was not printed.')
