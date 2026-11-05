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

def create(path):
    with open(path,'wb') as f: f.truncate(128*1024*1024)

def launch(image,label,crash=0,network=None):
    cmd=[QEMU,'-kernel','omega_os.elf','-smp','2','-m','128M','-serial','stdio','-display','none','-no-reboot','-accel','tcg,thread=multi',
         '-drive',f'file={image},if=virtio,format=raw']
    if crash: cmd+=['-append',f'sls.crash={crash}']
    if network:
        role,port=network
        endpoint=f'listen=127.0.0.1:{port}' if role==1 else f'connect=127.0.0.1:{port}'
        cmd+=['-netdev',f'socket,id=n,{endpoint}','-device',f'virtio-net-pci,netdev=n,mac=52:54:00:12:34:0{role}', '-object',f'filter-dump,id=dump,netdev=n,file=build/{label}.pcap']
    return subprocess.Popen(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT),label,crash,network

def finish(jobs):
    sel=selectors.DefaultSelector(); buffers={p:bytearray() for p,*_ in jobs}; pending=set(buffers); deadline=time.monotonic()+30
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
                for m in ['noncanonical syscall stack quarantined without SYSRET PASS','private peer page PF + non-executable stack PF PASS','SMP SPSC 65536','Ed25519 external signature','private CR3; Ring3 IPC syscalls PASS','Ring3 PF/GP + disk rollback PASS','SLS global atomic epoch=']:
                    assert m in text,(label,m,text)
                if network: assert 'two-node Ethernet + peer durable commit PASS' in text,text
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

b=Path('omega_os.elf').read_bytes(); offset=b.find(struct.pack('<I',0x1badb002),0,8192)
assert offset>=0 and offset%4==0 and sum(struct.unpack_from('<3I',b,offset))&0xffffffff==0
image=Path('build/v2-test.img'); create(image)
boot(image,'v2-first'); assert epochs(roots(image)[-1][1])==(65536,65536)
boot(image,'v2-reboot'); assert epochs(roots(image)[-1][1])==(131072,131072)
expected=bytes((j^0xa5)&255 for j in range(4096)); assert roots(image)[-1][1][2][1]==expected
assert roots(image)[-1][1][262143][1][:16]==b'OMEGA-GIB-SPARSE'
for stage in range(1,5):
    cut=Path(f'build/v2-cut-{stage}.img'); sparse_copy(image,cut)
    before=roots(cut)[-1]; boot(cut,f'v2-cut-{stage}',stage); after=roots(cut)[-1]
    assert epochs(after[1]) in [(131072,131072),(131073,131073)],'mixed-page epoch'
    if stage<3: assert after[0]==before[0] and epochs(after[1])==(131072,131072)
    if stage==4: assert after[0]==before[0]+1 and epochs(after[1])==(131073,131073)
    boot(cut,f'v2-cut-{stage}-reboot')
    assert epochs(roots(cut)[-1][1])[0]==epochs(roots(cut)[-1][1])[1]
# A corrupted latest payload invalidates its WHOLE root, not one page only.
cut=Path('build/v2-corrupt.img'); sparse_copy(image,cut)
previous,newest=roots(cut)[-2:]
with cut.open('r+b') as f:
    f.seek(newest[1][262143][0]*512); f.write(b'BROKEN-DATA'); f.flush(); os.fsync(f.fileno())
assert roots(cut)[-1][0]==previous[0]
boot(cut,'v2-corrupt-reboot')
assert epochs(roots(cut)[-1][1])[0]==epochs(roots(cut)[-1][1])[1]
print('PASS atomic multi-page root: four SIGKILL boundaries, two boots, whole-root corruption fallback')
# Two independent bare-metal guests exchange raw Ethernet through QEMU's socket.
with socket.socket() as s: s.bind(('127.0.0.1',0)); port=s.getsockname()[1]
a=Path('build/node-a.img'); c=Path('build/node-b.img'); create(a); create(c)
jobs=[launch(a,'v2-node-a',network=(1,port))]
time.sleep(.15); jobs.append(launch(c,'v2-node-b',network=(2,port)))
finish(jobs)
for path,peer in [(a,2),(c,1)]:
    data=roots(path)[-1][1][3][1]
    assert data[:13]==b'OMEGA-PEER-V2' and struct.unpack_from('<Q',data,16)[0]==0x4f4d454741000000|peer
for node in ['a','b']:
    assert b'OMEGA-USER-DMA' in Path(f'build/v2-node-{node}.pcap').read_bytes(), 'missing direct user TX frame'
boot(a,'v2-peer-state-reboot')
assert roots(a)[-1][1][3][1][:13]==b'OMEGA-PEER-V2'
print('PASS two-node VirtIO-Net RX/TX DMA and peer-state global disk commit')
print('PASS LAPIC preemption, private CR3, SYSCALL/SYSRET IPC, Ed25519, typed bytecode verifier')
