"""RFC8032 interoperability, negative signatures and type-aware VM corpus."""
import ctypes as C
import pathlib,random,subprocess,re
subprocess.run(['gcc','-shared','-fPIC','-O2','-std=c99','-Wall','-Wextra','-Werror','-pedantic','-Iinclude','-Iinclude/crypto','src/vm.c','src/crypto/monocypher.c','src/crypto/monocypher-ed25519.c','-o','build/admission.so'],check=True)
lib=C.CDLL('./build/admission.so')
verify=lib.crypto_ed25519_check; verify.argtypes=[C.c_char_p,C.c_char_p,C.c_char_p,C.c_size_t]; verify.restype=C.c_int
key=bytes.fromhex('d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a')
sig=bytes.fromhex('e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b')
assert verify(sig,key,b'',0)==0
for i in range(64):
    bad=bytearray(sig); bad[i]^=1; assert verify(bytes(bad),key,b'',0)!=0
assert verify(sig,key,b'x',1)!=0
header=pathlib.Path('build/capsule.h').read_text()
def array(name):
    body=re.search(name+r'\s*\[\s*\]\s*=\s*\{([^}]*)\}',header).group(1)
    return bytes(int(x.strip(),0) for x in body.split(',') if x.strip())
msg=array('capsule_message'); pub=array('authority_public'); signature=array('capsule_signature')
assert verify(signature,pub,msg,len(msg))==0
compiler=array('compiler_message');compiler_signature=array('compiler_signature')
assert verify(compiler_signature,pub,compiler,len(compiler))==0
for i in [0,16,20,len(msg)-1]:
    bad=bytearray(msg); bad[i]^=1; assert verify(signature,pub,bytes(bad),len(bad))!=0
vm=lib.vm_verify; vm.argtypes=[C.c_char_p,C.c_size_t]; vm.restype=C.c_bool
corpus=[(b'\x01\x01\x01\x02\x02\x01\x03\x03\x04\x00',True),
        (b'',False),(b'\x00',False),(b'\x02\x00',False),(b'\x01',False),
        (b'\x01\x01\x04\x00',False), # ASSERT requires boolean, not integer 1
        (b'\x01\x01\x01\x02\x03\x04\x00',False), # false assertion
        (b'\xff',False),(b'\x01\x01'*17+b'\x00',False),
        (b'\x01\x01\x01\x01\x03\x01\x01\x02\x00',False)]
for code,expected in corpus: assert vm(code,len(code))==expected
rng=random.Random(42)
for _ in range(1000):
    a=rng.randrange(128); b=rng.randrange(128)
    code=bytes([1,a,1,b,2,1,a+b,3,4,0]); assert vm(code,len(code))
    bad=code[:-3]+bytes([255]); assert not vm(bad,len(bad))
print('PASS RFC8032 + 69 tamper checks; VM typed negatives + 2000 arithmetic policies')
