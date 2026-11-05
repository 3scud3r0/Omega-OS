"""V2 hardware acceptance: atomic roots, timer/SYSCALL processes, Ed25519, P2P DMA."""
from pathlib import Path
import os,selectors,struct,subprocess,sys,time,socket
QEMU=sys.argv[1] if len(sys.argv)>1 else 'qemu-system-x86_64'
MASK=(1<<64)-1
MAGIC=0x324f4d454741534c

def crc(data):
    c=0
    for b in data:
        c^=b<<56
        for _ in range(8): c=((c<<1)^(0x42f0e1eba9ea3693 if c>>63 else 0))&MASK
    return c

def roots(image):
    candidates=[]
    with open(image,'rb') as f:
        def block(s,h,limit):
            assert s>=8 and s%8==0 and s+8<=limit
            f.seek(s*512); data=f.read(4096)
            assert len(data)==4096 and crc(data)==h
            return data
        def walk(s,h,depth,prefix,limit,pages):
            words=struct.unpack('<512Q',block(s,h,limit))
            for i in range(256):
                if words[2*i]:
                    if depth==2: pages[(prefix<<8)|i]=(words[2*i],block(words[2*i],words[2*i+1],limit))
                    else: walk(words[2*i],words[2*i+1],depth+1,(prefix<<8)|i,limit,pages)
        for slot in range(2):
            f.seek(slot*512); raw=bytearray(f.read(512))
            if len(raw)<512: continue
            v=struct.unpack('<64Q',raw); raw[40:48]=bytes(8)
            if v[0]!=MAGIC or not v[1] or crc(raw)!=v[5]: continue
            pages={}
            try: walk(v[2],v[3],0,0,v[4],pages)
            except AssertionError: continue
            candidates.append((v[1],pages))
    assert candidates,'no complete global root'
    return sorted(candidates)

def create(path,size=128*1024*1024):
    with open(path,'wb') as f: f.truncate(size)

def launch(image,label,crash=0,network=None,stress=False,source=None):
    cmd=[QEMU,'-kernel','omega_os.elf','-smp','2','-m','128M','-serial','stdio','-display','none','-no-reboot','-accel','tcg,thread=multi',
         '-drive',f'file={image},if=virtio,format=raw']
    if source is not None: cmd+=['-append','omega.source='+source]
    if stress: cmd+=['-append','sls.stress']
    if crash: cmd+=['-append',f'sls.crash={crash}']
    if network:
        role,port,config=network
        cmd+=['-cpu','max','-initrd',str(config),'-netdev',f'socket,id=n,connect=127.0.0.1:{port}',
              '-device',f'virtio-net-pci,netdev=n,mac=52:54:00:12:34:0{role}',
              '-object',f'filter-dump,id=dump,netdev=n,file=build/{label}.pcap']
    return subprocess.Popen(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT),label,crash,network

def finish(jobs):
    sel=selectors.DefaultSelector(); buffers={p:bytearray() for p,*_ in jobs}; pending=set(buffers); deadline=time.monotonic()+90
    for p,*_ in jobs: sel.register(p.stdout,selectors.EVENT_READ,p)
    try:
        while pending and time.monotonic()<deadline:
            for key,_ in sel.select(.1):
                p=key.data; chunk=os.read(key.fd,65536)
                if chunk: buffers[p].extend(chunk)
                if b'FAIL:' in buffers[p] or (not chunk and p.poll() is not None): raise AssertionError(buffers[p].decode(errors='replace'))
                job=next(j for j in jobs if j[0] is p)
                marker=f'SLS crash-test boundary={job[2]}\n'.encode() if job[2] else b'complete; HLT\n'
                if marker in buffers[p]: pending.discard(p); sel.unregister(p.stdout)
        assert not pending,'QEMU progress timeout: '+repr([buffers[p].decode(errors='replace') for p in pending])
        texts=[]
        for p,label,crash,network in jobs:
            text=buffers[p].decode(errors='replace'); texts.append(text)
            if not crash:
                for m in ['noncanonical syscall stack quarantined without SYSRET PASS','private peer page PF + non-executable stack PF PASS','SMP SPSC 65536','Ed25519 external signature','private CR3; Ring3 IPC syscalls PASS','original Omega frontend + egraph + tensor PUCT in Ring3 + dynamic child + exact SLS PASS','Ring3 PF/GP + disk rollback PASS','SLS global atomic epoch=']:
                    assert m in text,(label,m,text)
                if network: assert 'routed encrypted UDP + authenticated 4096-byte SLS replication PASS' in text,text
        return texts
    finally:
        for p,label,*_ in jobs:
            if p.poll() is None: p.kill()
            p.wait(timeout=5); Path(f'build/{label}.log').write_bytes(buffers[p])
        sel.close()

def boot(image,label,crash=0): return finish([launch(image,label,crash)])[0]
def epochs(pages): return tuple(struct.unpack_from('<Q',pages[i][1],80)[0] for i in [0,1])
def sparse_copy(source,target):
    subprocess.run(['cp','--sparse=always',str(source),str(target)],check=True)
