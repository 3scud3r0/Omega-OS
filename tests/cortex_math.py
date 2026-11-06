"""Exact integer paths, all FP16 encodings and GGML quantized block layouts."""
import ctypes as C
import math,random,struct,subprocess
import numpy as np
subprocess.run(['gcc','-std=c99','-O3','-Wall','-Wextra','-Werror','-pedantic','-ffreestanding','-fno-builtin','-shared','-fPIC','-DCORTEX_HOST_TEST','-Iinclude','user/omega/cortex.c','src/rational.c','-o','build/cortex_math.so'],check=True)
lib=C.CDLL('./build/cortex_math.so');lib.cortex_fp16.argtypes=[C.c_uint16];lib.cortex_fp16.restype=C.c_float
lib.cortex_dequant_value.argtypes=[C.c_uint,C.c_void_p,C.c_uint];lib.cortex_dequant_value.restype=C.c_float
lib.cortex_gemm_i8.argtypes=[C.c_void_p,C.c_void_p,C.c_void_p,C.c_uint,C.c_uint,C.c_uint,C.c_bool];lib.cortex_gemm_i8.restype=C.c_bool
lib.cortex_test_sin.argtypes=[C.c_float];lib.cortex_test_sin.restype=C.c_float
lib.cortex_test_exp.argtypes=[C.c_float];lib.cortex_test_exp.restype=C.c_float
for angle in [0.0,1.0,-1.0,3.1415927,100.0,12345.0,1000000.0,-1000000.0]:
    x=C.c_float(angle).value;assert abs(lib.cortex_test_sin(x)-math.sin(x))<4e-7,(angle,lib.cortex_test_sin(x),math.sin(x))
for x in range(-70,71):assert abs(lib.cortex_test_exp(x)/math.exp(x)-1)<8e-6
for h in range(65536):
    expected=struct.unpack('<e',struct.pack('<H',h))[0];got=lib.cortex_fp16(h)
    assert (math.isnan(expected) and math.isnan(got)) or expected==got,(h,got,expected)
rng=np.random.default_rng(7)
for k in [1,17,32,257,1041]:
    a=rng.integers(-128,128,(7,k),dtype=np.int8);b=rng.integers(-128,128,(43,k),dtype=np.int8);expected=a.astype(np.int64)@b.astype(np.int64).T
    for vector in [False,True]:
        actual=np.empty((7,43),dtype=np.int64);assert lib.cortex_gemm_i8(a.ctypes.data,b.ctypes.data,actual.ctypes.data,7,43,k,vector);assert np.array_equal(actual,expected)
from cortex_fixture import model
for mode in ['q8','q4']:
    decoded=model(f'build/cortex-math-{mode}.gguf',mode)
    # Parse GGUF tensor offsets independently with the fixture's specification.
    raw=open(f'build/cortex-math-{mode}.gguf','rb').read();position=24;nt,nm=struct.unpack_from('<QQ',raw,8)
    def string():
        global position
        n=struct.unpack_from('<Q',raw,position)[0];position+=8;s=raw[position:position+n];position+=n;return s.decode()
    for _ in range(nm):
        string();kind=struct.unpack_from('<I',raw,position)[0];position+=4
        if kind==8:string()
        else:position+=4
    tensors=[]
    for _ in range(nt):
        name=string();rank=struct.unpack_from('<I',raw,position)[0];position+=4;position+=rank*8;kind,offset=struct.unpack_from('<IQ',raw,position);position+=12;tensors.append((name,kind,offset))
    start=(position+31)&~31
    for name,kind,offset in tensors:
        count=32 if kind==8 else (256 if kind in [12,14] else 1)
        buffer=C.create_string_buffer(raw[start+offset:start+offset+max(210,count*4)])
        expected=decoded[name][2][:count]
        for i,want in enumerate(expected):
            got=lib.cortex_dequant_value(kind,buffer,i);assert abs(got-want)<2e-5,(mode,name,i,got,want)
# High bits of Q4 scale/min and full signed Q6 scales, independently decoded.
random_source=random.Random(817)
for case in range(32):
    d=struct.unpack('<e',struct.pack('<e',0.003))[0];dm=struct.unpack('<e',struct.pack('<e',0.002))[0]
    scales=bytes(random_source.randrange(256) for _ in range(12));quants=bytes(random_source.randrange(256) for _ in range(128));block=struct.pack('<ee',d,dm)+scales+quants;buffer=C.create_string_buffer(block)
    for index in range(256):
        group,j=divmod(index,32)
        if group<4:scale,minimum=scales[group]&63,scales[group+4]&63
        else:scale=(scales[group+4]&15)+16*(scales[group-4]>>6);minimum=(scales[group+4]>>4)+16*(scales[group]>>6)
        quant=(quants[group//2*32+j]>>(4*(group%2)))&15
        assert abs(lib.cortex_dequant_value(12,buffer,index)-(d*scale*quant-dm*minimum))<2e-5
    low=bytes(random_source.randrange(256) for _ in range(128));high=bytes(random_source.randrange(256) for _ in range(64));signed=[random_source.randint(-128,127) for _ in range(16)];block=low+high+struct.pack('<16be',*signed,d);buffer=C.create_string_buffer(block)
    for index in range(256):
        part,inside=divmod(index,128);group,j=divmod(inside,32);nibble=low[part*64+j+(group%2)*32];nibble=(nibble&15) if group<2 else nibble>>4;quant=(nibble|(((high[part*32+j]>>(group*2))&3)<<4))-32
        want=d*signed[part*8+group*2+j//16]*quant
        assert abs(lib.cortex_dequant_value(14,buffer,index)-want)<2e-5
class Q(C.Structure):_fields_=[('num',C.c_longlong),('den',C.c_longlong)]
class Rule(C.Structure):_fields_=[('token',C.c_uint),('reserved',C.c_uint),('value',Q)]
lib.cortex_symbolic_prune.argtypes=[C.c_void_p,C.c_uint,C.c_void_p,C.c_uint];lib.cortex_symbolic_prune.restype=C.c_uint
scores=(C.c_float*8)(*range(8));rules=(Rule*3)(Rule(0,0,Q(1,2)),Rule(1,0,Q(3,2)),Rule(3,0,Q(-1,2)))
assert lib.cortex_symbolic_prune(scores,8,rules,3)==6
assert scores[0]==0 and scores[3]==3 and all(math.isinf(scores[i]) and scores[i]<0 for i in [1,2,4,5,6,7])
print('PASS Cortex math: all 65536 FP16 encodings; exact tiled scalar/AVX integer GEMM; Q8_0/Q4_K/Q6_K reference blocks')
