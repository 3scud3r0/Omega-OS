"""Independent X25519 and RFC8439 interoperability, durable-epoch/replay policies."""
import ctypes as C
import hashlib,struct,subprocess
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
subprocess.run(['gcc','-shared','-fPIC','-O2','-Wall','-Wextra','-Werror','-pedantic','-std=c99','-Iinclude','-Iinclude/crypto','src/tunnel.c','src/crypto/monocypher.c','-o','build/tunnel.so'],check=True)
class T(C.Structure):
    _fields_=[(x,C.c_ubyte*n) for x,n in [('auth',32),('secret',32),('public_key',32),('nonce',16),('peer_public',32),('peer_nonce',16),('session',16),('tx',32),('rx',32),('saved_hello',32)]]+[(x,C.c_ulonglong) for x in ['boot','peer_boot','min_peer','sequence','highest','window','replay_drops','auth_drops']]+[('local_ip',C.c_uint),('peer_ip',C.c_uint),('established',C.c_bool)]
lib=C.CDLL('./build/tunnel.so')
lib.tunnel_init.argtypes=[C.POINTER(T)]+[C.c_void_p]*3+[C.c_ulonglong,C.c_ulonglong,C.c_void_p,C.c_uint,C.c_uint];lib.tunnel_init.restype=C.c_bool
lib.tunnel_hello.argtypes=[C.POINTER(T),C.c_void_p]
lib.tunnel_accept.argtypes=[C.POINTER(T),C.c_void_p,C.c_size_t,C.POINTER(C.c_bool)];lib.tunnel_accept.restype=C.c_bool
lib.tunnel_seal.argtypes=[C.POINTER(T),C.c_void_p,C.c_void_p,C.c_size_t];lib.tunnel_seal.restype=C.c_size_t
lib.tunnel_open.argtypes=[C.POINTER(T),C.c_void_p,C.POINTER(C.c_size_t),C.c_void_p,C.c_size_t];lib.tunnel_open.restype=C.c_bool
ka=bytes(range(32));kb=bytes(range(32,64));pa=X25519PrivateKey.from_private_bytes(ka).public_key().public_bytes_raw();pb=X25519PrivateKey.from_private_bytes(kb).public_key().public_bytes_raw()
def init(secret,peer,random,boot=1,floor=0,saved=bytes(32),ip=1):
    state=T();assert lib.tunnel_init(C.byref(state),secret,peer,random,boot,floor,saved,ip,3-ip);return state

def hello(t):
    out=C.create_string_buffer(104);lib.tunnel_hello(C.byref(t),out);return out.raw

def accept(t,h,expected=True):
    changed=C.c_bool();ok=lib.tunnel_accept(C.byref(t),h,len(h),C.byref(changed));assert ok==expected;return changed.value

def seal(t,data):
    out=C.create_string_buffer(1200);n=lib.tunnel_seal(C.byref(t),out,data,len(data));assert n==len(data)+56;return out.raw[:n]

def opened(t,p,expected=True):
    out=C.create_string_buffer(1100);n=C.c_size_t();ok=lib.tunnel_open(C.byref(t),out,C.byref(n),p,len(p));assert ok==expected;return out.raw[:n.value]
a=init(ka,pb,bytes(range(48)),ip=1);b=init(kb,pa,bytes(range(48,96)),ip=2)
shared=X25519PrivateKey.from_private_bytes(ka).exchange(X25519PrivateKey.from_private_bytes(kb).public_key())
assert bytes(a.auth)==hashlib.blake2b(b'OM-AUTH-V3'+bytes(6),key=shared,digest_size=32).digest()
ha,hb=hello(a),hello(b);assert accept(a,hb) and accept(b,ha)
assert bytes(a.tx)==bytes(b.rx) and bytes(a.rx)==bytes(b.tx)
packet=seal(a,b'authenticated state')
assert packet[40:]==ChaCha20Poly1305(bytes(a.tx)).encrypt(bytes(4)+packet[16:24],b'authenticated state',packet[:40])
for index in [10,16,40,len(packet)-1]:
    bad=bytearray(packet);bad[index]^=1;opened(b,bytes(bad),False);assert b.highest==0
assert opened(b,packet)==b'authenticated state';opened(b,packet,False)
p2,p3,p4=[seal(a,bytes([x])) for x in [2,3,4]]
assert opened(b,p4)==b'\x04' and opened(b,p2)==b'\x02';opened(b,p2,False);assert opened(b,p3)==b'\x03'
assert not accept(b,ha);opened(b,packet,False) # HELLO must not reset sequence window.
old_packet=seal(a,b'old session')
a2=init(ka,pb,bytes(range(96,144)),boot=2,ip=1);assert accept(b,hello(a2));accept(b,ha,False);assert accept(a2,hb)
opened(b,old_packet,False);assert opened(b,seal(a2,b'new session'))==b'new session'
b2=init(kb,pa,bytes(range(144,192)),boot=2,floor=2,saved=bytes(b.saved_hello),ip=2)
accept(b2,ha,False);assert accept(b2,hello(a2));assert accept(a2,hello(b2));assert opened(b2,seal(a2,b'reboot'))==b'reboot'
for _ in range(80):last=seal(a2,b'x')
assert opened(b2,last)==b'x';opened(b2,packet,False)
print('PASS X25519 independent exchange; RFC8439 ChaCha20-Poly1305 interoperability; forgery, replay, out-of-order, HELLO and reboot floors')
