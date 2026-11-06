"""Execute the actual x86-64 emitter with protected executable mappings."""
import ctypes as C
from fractions import Fraction
import mmap
from pathlib import Path
import random
import struct
import subprocess
root=Path(__file__).resolve().parents[1]
subprocess.run(['gcc','-std=c99','-O3','-Wall','-Wextra','-Werror','-pedantic','-fPIC','-shared','-Iinclude','src/jit.c','src/rational.c','src/bytecode.c','-o','build/jit_test.so'],cwd=root,check=True)
lib=C.CDLL(str(root/'build/jit_test.so'))
lib.jit_compile.argtypes=[C.c_void_p,C.c_size_t,C.c_void_p,C.POINTER(C.c_size_t)]
lib.jit_compile.restype=C.c_bool
lib.jit_audit.argtypes=[C.c_void_p,C.c_size_t,C.c_void_p,C.c_size_t]
lib.jit_audit.restype=C.c_bool
libc=C.CDLL(None);libc.mprotect.argtypes=[C.c_void_p,C.c_size_t,C.c_int]
class Q(C.Structure):_fields_=[('num',C.c_longlong),('den',C.c_longlong)]
def run(ops,expected):
    cert=struct.pack('<QQ',0x3343424f,len(ops)+1)+ops+b'\0'
    mapping=mmap.mmap(-1,4096,prot=mmap.PROT_READ|mmap.PROT_WRITE)
    view=(C.c_ubyte*4096).from_buffer(mapping);addr=C.addressof(view);n=C.c_size_t()
    assert lib.jit_compile(cert,len(cert),addr,C.byref(n))
    assert lib.jit_audit(cert,len(cert),addr,n.value)
    locations=[0,4095]
    branch=bytes(view).find(b'\x0f\x80')
    if branch>=0:locations.append(branch+2)
    for location in locations:
        original=view[location];view[location]^=0x80
        assert not lib.jit_audit(cert,len(cert),addr,n.value)
        view[location]=original
    assert libc.mprotect(addr,4096,mmap.PROT_READ|mmap.PROT_EXEC)==0
    q=Q();ok=C.CFUNCTYPE(C.c_bool,C.POINTER(Q))(addr)(C.byref(q))
    assert ok and Fraction(q.num,q.den)==expected,(q.num,q.den,expected)
    assert libc.mprotect(addr,4096,mmap.PROT_READ|mmap.PROT_WRITE)==0
    del view;mapping.close()
def push(q):return b'\1'+struct.pack('<qq',q.numerator,q.denominator)
rng=random.Random(4)
for i in range(1000):
    a,b=Fraction(rng.randint(-100,100),rng.randint(1,100)),Fraction(rng.randint(-100,100),rng.randint(1,100))
    op=rng.choice([2,3,4,5] if b else [2,3,4])
    expected={2:lambda:a+b,3:lambda:a-b,4:lambda:a*b,5:lambda:a/b}[op]()
    run(push(a)+push(b)+bytes([op]),expected)
run(push(Fraction(-(1<<63),1)),Fraction(-(1<<63),1))
run(push(Fraction(0xfa,1)),Fraction(0xfa,1)) # forbidden byte inside literal is data
print('JIT PASS: 1002 native rational executions; full-page canonical audit; host W^X')
