"""Two routed subnets, ARP gateways, authenticated encrypted UDP, adverse frames."""
from hardware import *
import hashlib,ipaddress,threading
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey

def checksum(data):
    if len(data)&1:data+=b'\0'
    value=sum(struct.unpack('!%dH'%(len(data)//2),data))
    while value>>16:value=(value&65535)+(value>>16)
    return (~value)&65535

class Router:
    def __init__(self):
        self.selector=selectors.DefaultSelector();self.listeners=[];self.connections={};self.input={};self.output={};self.inject=[0,0];self.count=[0,0];self.capture=[];self.stop=False;self.error=None
        self.ips=[int(ipaddress.IPv4Address(x)) for x in ['10.21.0.2','10.22.0.2']]
        self.gateways=[int(ipaddress.IPv4Address(x)) for x in ['10.21.0.1','10.22.0.1']]
        for i in range(2):
            s=socket.socket();s.bind(('127.0.0.1',0));s.listen();s.setblocking(False);self.listeners.append(s);self.selector.register(s,selectors.EVENT_READ,('listen',i))
        self.ports=[s.getsockname()[1] for s in self.listeners]
        self.thread=threading.Thread(target=self.run,daemon=True);self.thread.start()
    def enqueue(self,i,packet):
        if i not in self.connections:return
        self.output[i]+=struct.pack('!I',len(packet))+packet
        self.selector.modify(self.connections[i],selectors.EVENT_READ|selectors.EVENT_WRITE,('connection',i))
    def route(self,i,p):
        if len(p)<42:return
        other=1-i;kind=struct.unpack_from('!H',p,12)[0]
        gateway_mac=bytes([0x52,0x54,0,0xab,0,0xe1+i])
        if kind==0x806 and struct.unpack_from('!H',p,20)[0]==1:
            target=struct.unpack_from('!I',p,38)[0]
            assert target==self.gateways[i],'guest failed to choose off-subnet gateway'
            reply=bytearray(60);reply[:6]=p[6:12];reply[6:12]=gateway_mac;reply[12:22]=bytes.fromhex('08060001080006040002')
            reply[22:28]=gateway_mac;reply[28:32]=struct.pack('!I',self.gateways[i]);reply[32:38]=p[22:28];reply[38:42]=p[28:32]
            self.enqueue(i,reply);return
        if kind!=0x800:return # V2 direct-DMA probe isn't part of secure state traffic.
        assert checksum(p[14:34])==0 and p[23]==17
        assert struct.unpack_from('!2I',p,26)==(self.ips[i],self.ips[other])
        body=p[42:];assert b'OMEGA-COG-V3' not in body,'plaintext snapshot leaked'
        self.capture.append(p)
        routed=bytearray(p);routed[:6]=bytes([0x52,0x54,0,0x12,0x34,other+1]);routed[6:12]=bytes([0x52,0x54,0,0xab,0,0xe1+other]);routed[22]-=1;routed[24:26]=b'\0\0';routed[24:26]=struct.pack('!H',checksum(bytes(routed[14:34])))
        if len(body)>8 and body[:8]==b'OMPNET3\0' and body[8]==2:
            self.count[i]+=1
            if self.inject[i]<8:
                bad=bytearray(routed);bad[-1]^=0x80;bad[40:42]=b'\0\0';udp_length=struct.unpack_from('!H',bad,38)[0]
                pseudo=bytes(bad[26:34])+b'\0\x11'+bytes(bad[38:40]);bad[40:42]=struct.pack('!H',checksum(pseudo+bytes(bad[34:34+udp_length])) or 65535)
                self.enqueue(other,bad);self.enqueue(other,routed);self.enqueue(other,routed);self.inject[i]+=1;return
        self.enqueue(other,routed)
    def run(self):
        try:
            while not self.stop:
                for key,events in self.selector.select(.05):
                    mode,i=key.data
                    if mode=='listen':
                        conn,_=key.fileobj.accept();conn.setblocking(False);self.connections[i]=conn;self.input[i]=bytearray();self.output[i]=bytearray();self.selector.register(conn,selectors.EVENT_READ,('connection',i));continue
                    conn=key.fileobj
                    if events&selectors.EVENT_READ:
                        try:data=conn.recv(65536)
                        except ConnectionResetError:data=b''
                        if not data:self.selector.unregister(conn);conn.close();self.connections.pop(i,None);continue
                        self.input[i]+=data
                        while len(self.input[i])>=4:
                            n=struct.unpack_from('!I',self.input[i])[0]
                            assert n<=65536
                            if len(self.input[i])<n+4:break
                            p=bytes(self.input[i][4:4+n]);del self.input[i][:4+n]
                            if n:self.route(i,p)
                    if events&selectors.EVENT_WRITE and self.output[i]:
                        try:sent=conn.send(self.output[i])
                        except (BrokenPipeError,ConnectionResetError):
                            self.selector.unregister(conn);conn.close();self.connections.pop(i,None);continue
                        del self.output[i][:sent]
                        if not self.output[i]:self.selector.modify(conn,selectors.EVENT_READ,('connection',i))
        except (OSError,AssertionError) as exc:
            if not self.stop:self.error=exc
    def close(self):
        self.stop=True;self.thread.join(timeout=2)
        for s in [*self.listeners,*self.connections.values()]:s.close()
        self.selector.close()

def configs():
    import tempfile
    root=Path(tempfile.mkdtemp(prefix='peer-config-',dir='build'));result=[]
    for i in range(2):
        subprocess.run([sys.executable,'tools/peer_config.py','keygen','--private',str(root/f'node-{i}.key'),'--public',str(root/f'node-{i}.pub')],check=True,stdout=subprocess.DEVNULL)
    for i in range(2):
        path=root/f'node-{i}.conf'
        subprocess.run([sys.executable,'tools/peer_config.py','config','--private-file',str(root/f'node-{i}.key'),'--peer-public-file',str(root/f'node-{1-i}.pub'),
                        '--local-ip',f'10.{21+i}.0.2','--peer-ip',f'10.{22-i}.0.2','--gateway',f'10.{21+i}.0.1','--output',str(path)],check=True,stdout=subprocess.DEVNULL)
        assert path.stat().st_mode&0o777==0o600
        result.append(path)
    return result

if __name__=='__main__':
    configuration=configs();images=[Path('build/v3-node-a.img'),Path('build/v3-node-b.img')]
    for image in images:create(image)
    for boot_number in [1,2]:
        router=Router()
        try:
            jobs=[launch(images[i],f'v3-net-{boot_number}-{i}',network=(i+1,router.ports[i],configuration[i])) for i in range(2)]
            text=finish(jobs)
            assert router.error is None,router.error
            assert min(router.count)>4 and min(router.inject)>0
            for output in text:
                import re
                replay,auth=map(int,re.search(r'rejected replay=(\d+) authentication=(\d+)',output).groups());assert replay>0 and auth>0,output
        finally:router.close()
        a,b=[roots(p)[-1][1] for p in images]
        assert a[4][1]==b[5][1] and b[4][1]==a[5][1],'replicated cognitive pages differ'
        for pages in [a,b]:
            local,peer=struct.unpack_from('<2Q',pages[3][1],16);assert local==boot_number and peer==boot_number
    print('PASS two different IPv4 subnets via ARP gateways; X25519 + RFC8439 AEAD; injected forgery/replay rejected; full SLS page replication and reboot counters')
