"""Provision an existing GGUF into a NEW SLS image, streaming 4-KiB pages.
No model is downloaded or silently replaced. Private images are never bundled.
"""
import argparse,hashlib,os,struct,json
from pathlib import Path
META=0x1000;MODEL=0x2000
POLY=0x42F0E1EBA9EA3693;MASK=(1<<64)-1
TABLE=[]
for i in range(256):
    c=i<<56
    for _ in range(8):c=((c<<1)^(POLY if c>>63 else 0))&MASK
    TABLE.append(c)
def crc(data):
    c=0
    for b in data:c=TABLE[(c>>56)^b]^((c<<8)&MASK)
    return c

def provision(gguf,image,size,steps,prompt,contracts=(),stress_pages=0):
    length=gguf.stat().st_size
    if not 0<length<(0x800000-MODEL)*4096:raise ValueError('GGUF exceeds SLS model namespace')
    if size<4*length+1024*1024:raise ValueError('Need room for two SLS semispaces and transaction reserve')
    if not 0<steps<=1048576 or len(prompt)>128 or any(not 0<=x<262144 for x in prompt):raise ValueError('Invalid token schedule')
    with gguf.open('rb') as source:
        if source.read(8)!=b'GGUF\x03\0\0\0':raise ValueError('Expected a GGUF v3 file')
        source.seek(0);digest=hashlib.blake2b(digest_size=32)
        while block:=source.read(1024*1024):digest.update(block)
    fd=os.open(image,os.O_RDWR|os.O_CREAT|os.O_EXCL,0o600)
    try:
        with os.fdopen(fd,'r+b') as disk:
            disk.truncate(size);cursor=8;leaves={}
            def append(data):
                nonlocal cursor
                assert len(data)==4096
                if (cursor+8)*512>size//2:raise ValueError('Live pages exceed one semispace')
                disk.seek(cursor*512);disk.write(data);entry=(cursor,crc(data));cursor+=8;return entry
            def put(index,data):
                leaf=leaves.setdefault(index>>8,[0]*512);leaf[(index&255)*2:(index&255)*2+2]=append(data)
            header=bytearray(4096);header[:16]=b'OMEGA-CORTEX-01\0';struct.pack_into('<Q32sII',header,16,length,digest.digest(),steps,len(prompt))
            for i,t in enumerate(prompt):struct.pack_into('<I',header,64+i*4,t)
            if len(contracts)>16:raise ValueError("At most 16 explicit logical token contracts")
            struct.pack_into("<I",header,576,len(contracts))
            for i,(token,num,den) in enumerate(contracts):struct.pack_into("<IIqq",header,580+24*i,token,0,num,den)
            if not 0<=stress_pages<=32768:raise ValueError("Pager stress page count")
            struct.pack_into("<I",header,1000,stress_pages)
            put(META,header)
            with gguf.open('rb') as source:
                index=MODEL
                while data:=source.read(4096):put(index,data.ljust(4096,b'\0'));index+=1
            directories={}
            for prefix,leaf in sorted(leaves.items()):
                node=directories.setdefault(prefix>>8,[0]*512);node[(prefix&255)*2:(prefix&255)*2+2]=append(struct.pack('<512Q',*leaf))
            root=[0]*512
            for i,node in sorted(directories.items()):root[i*2:i*2+2]=append(struct.pack('<512Q',*node))
            sector,hash_value=append(struct.pack('<512Q',*root));superblock=bytearray(512)
            struct.pack_into('<7Q',superblock,0,0x324f4d454741534c,1,sector,hash_value,cursor,0,3);struct.pack_into('<Q',superblock,40,crc(superblock))
            for slot in [0,1]:disk.seek(slot*512);disk.write(superblock)
            disk.flush();os.fsync(disk.fileno())
    except BaseException:
        image.unlink(missing_ok=True);raise
    return digest.digest()
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('gguf',type=Path);p.add_argument('image',type=Path);p.add_argument('--size-mib',type=int,default=4096);p.add_argument('--steps',type=int,default=1);p.add_argument('--prompt',default='1');p.add_argument('--contracts',type=Path);a=p.parse_args()
    rules=[] if a.contracts is None else [(x['token'],x['num'],x['den']) for x in json.loads(a.contracts.read_text())]
    provision(a.gguf,a.image,a.size_mib*1024*1024,a.steps,[int(x) for x in a.prompt.split(',') if x],rules);print('GGUF provisioned in new CoW SLS image; existing images were not overwritten.')
