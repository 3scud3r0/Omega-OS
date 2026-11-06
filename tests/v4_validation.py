"""Three routed QEMU nodes: native Ring3, bounded evolution, signed majority, SIGKILL."""
from hardware import *
from v3_network import Router,checksum
import hashlib,ipaddress,re,tempfile,threading
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from cluster_config import provision
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

class ClusterRouter(Router):
    def __init__(self):
        self.selector=selectors.DefaultSelector();self.listeners=[];self.connections={};self.input={};self.output={};self.capture=[];self.stop=False;self.error=None
        self.blocked={0,1,2}
        self.ips=[int(ipaddress.IPv4Address(f'10.{21+i}.0.2')) for i in range(3)]
        self.gateways=[int(ipaddress.IPv4Address(f'10.{21+i}.0.1')) for i in range(3)]
        for i in range(3):
            s=socket.socket();s.bind(('127.0.0.1',0));s.listen();s.setblocking(False);self.listeners.append(s);self.selector.register(s,selectors.EVENT_READ,('listen',i))
        self.ports=[s.getsockname()[1] for s in self.listeners];self.thread=threading.Thread(target=self.run,daemon=True);self.thread.start()
    def route(self,i,p):
        if len(p)<42:return
        kind=struct.unpack_from('!H',p,12)[0];gateway_mac=bytes([0x52,0x54,0,0xab,0,0xe1+i])
        if kind==0x806 and struct.unpack_from('!H',p,20)[0]==1:
            assert struct.unpack_from('!I',p,38)[0]==self.gateways[i]
            reply=bytearray(60);reply[:6]=p[6:12];reply[6:12]=gateway_mac;reply[12:22]=bytes.fromhex('08060001080006040002')
            reply[22:28]=gateway_mac;reply[28:32]=struct.pack('!I',self.gateways[i]);reply[32:38]=p[22:28];reply[38:42]=p[28:32];self.enqueue(i,reply);return
        if kind!=0x800:return
        assert checksum(p[14:34])==0 and p[23]==17
        src,dst=struct.unpack_from('!2I',p,26);assert src==self.ips[i] and dst in self.ips
        other=self.ips.index(dst)
        assert b'OMEGA-LINEAGE-V4' not in p[42:],'plaintext genome escaped tunnel'
        self.capture.append(p)
        if i in self.blocked or other in self.blocked:return
        routed=bytearray(p);routed[:6]=bytes([0x52,0x54,0,0x12,0x34,other+1]);routed[6:12]=bytes([0x52,0x54,0,0xab,0,0xe1+other]);routed[22]-=1;routed[24:26]=b'\0\0';routed[24:26]=struct.pack('!H',checksum(bytes(routed[14:34])));self.enqueue(other,routed)

def verify_record(raw,config):
    assert len(raw)==4096 and raw[:8]==b'LINEAGE4'
    epoch=struct.unpack_from('<Q',raw,8)[0];digest=hashlib.blake2b(raw[280:3096],digest_size=32).digest()
    assert digest==raw[48:80] and raw[280:296]==b'OMEGA-LINEAGE-V4'
    mask=struct.unpack_from('<Q',raw,80)[0];assert mask&1 and mask.bit_count()>=2 and mask&~7==0
    module=config.read_bytes();statement=b'OMEGA-QUORUM-V4\0'+module[96:128]+raw[8:16]+raw[16:48]+digest
    for i in range(3):
        if mask>>i&1:Ed25519PublicKey.from_public_bytes(module[128+i*80+48:128+i*80+80]).verify(raw[88+i*64:152+i*64],statement)
    return epoch,digest

def main():
    router=ClusterRouter();configuration=provision(Path(tempfile.mkdtemp(prefix='cluster-',dir='build')))
    images=[Path(f'build/v4-node-{i+1}.img') for i in range(3)]
    for image in images:create(image,32*1024*1024)
    jobs=[];logs={};selector=selectors.DefaultSelector()
    def start(i,label):
        job=launch(images[i],label,network=(i+1,router.ports[i],configuration[i]));jobs.append(job);logs[job[0]]=bytearray();selector.register(job[0].stdout,selectors.EVENT_READ,job);return job
    def until(job,marker,timeout=90):
        end=time.monotonic()+timeout
        while marker not in logs[job[0]] and time.monotonic()<end:
            assert router.error is None,router.error
            for key,_ in selector.select(.05):
                active=key.data;chunk=os.read(key.fd,65536)
                if chunk:logs[active[0]]+=chunk
                elif active[0].poll() is not None:selector.unregister(active[0].stdout)
                assert b'FAIL:' not in logs[active[0]],logs[active[0]].decode(errors='replace')
        assert marker in logs[job[0]],logs[job[0]].decode(errors='replace')
    try:
        a=start(0,'v4-node-1');until(a,b'QUORUM isolated: lineage epoch remains 0')
        assert 6 not in roots(images[0])[-1][1],'isolated leader published lineage'
        a[0].kill();a[0].wait(timeout=5)
        a=start(0,'v4-node-1-restart');until(a,b'QUORUM recovered durable uncommitted proposal')
        until(a,b'QUORUM service ready')
        b=start(1,'v4-node-2');c=start(2,'v4-node-3')
        until(b,b'QUORUM service ready');until(c,b'QUORUM service ready')
        # All three VMs now run concurrently in distinct routed subnets.
        router.blocked={2} # The third node remains alive but partitioned.
        for job in [a,b]:until(job,b'QUORUM durable lineage epoch=1')
        assert 6 not in roots(images[2])[-1][1]
        c[0].kill();c[0].wait(timeout=5) # actual SIGKILL before its publication
        router.blocked.clear()
        recovered=start(2,'v4-node-3-catchup');until(recovered,b'QUORUM durable lineage epoch=1')
        records=[roots(image)[-1][1][6][1] for image in images]
        assert len(set(records))==1
        for i,record in enumerate(records):assert verify_record(record,configuration[i])[0]==1
        for job in [a,b,c,recovered]:
            text=logs[job[0]].decode();native,reference=map(int,re.search(r'JIT native TSC=(\d+) reference interpreter TSC=(\d+)',text).groups());assert native<reference
            cycles,promoted,rejected=map(int,re.search(r'INCUBATOR cycles=(\d+) promotions=(\d+) rejected=(\d+)',text).groups());assert cycles>=50 and promoted>0 and rejected>=3
            assert 'egraph/PUCT mutations=50' in text
            assert 'TEST JIT RX write trapped #PF; child quarantined PASS' in text and '16 private Ring3 address spaces runnable simultaneously' in text and 'JIT fault rollback parent retained vector=14' in text
        recovered[0].kill();recovered[0].wait(timeout=5)
        restarted=start(2,'v4-node-3-restart');until(restarted,b'QUORUM recovered certified lineage epoch=1')
        assert roots(images[2])[-1][1][6][1]==records[2]
        for job in [a,b,restarted]:until(job,b'QUORUM durable lineage epoch=2')
        current_records=[roots(image)[-1][1][6][1] for image in images]
        assert len(set(current_records))==1
        for i,record in enumerate(current_records):assert verify_record(record,configuration[i])[0]==2
        for job in [a,b,restarted]:until(job,b'INCUBATOR cycles=100 ')
        assert len(router.capture)>10 and router.error is None
        print('PASS native JIT + 50 Pareto cycles + 16 private processes; isolated epoch blocked; 3 routed encrypted nodes; Ed25519 2/3 certificate; real SIGKILL recovery; persistent compiler resumed through epoch 2')
    finally:
        for job in jobs:
            if job[0].poll() is None:job[0].kill()
            job[0].wait(timeout=5);Path(f'build/{job[1]}.log').write_bytes(logs[job[0]])
        router.close();selector.close()
if __name__=='__main__':main()
