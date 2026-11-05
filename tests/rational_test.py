"""Cross-check freestanding arithmetic against Python exact fractions."""
import ctypes as C
from fractions import Fraction
import random
import subprocess

subprocess.run(['gcc', '-shared', '-fPIC', '-O3', '-Wall', '-Wextra', '-Werror',
                '-std=c99', '-Iinclude', 'src/rational.c', '-o', 'build/rational.so'], check=True)
class R(C.Structure):
    _fields_ = [('num', C.c_longlong), ('den', C.c_longlong)]
lib = C.CDLL('./build/rational.so')
for name in ['add', 'sub', 'mul']:
    f = getattr(lib, 'rational_' + name)
    f.argtypes = [R, R, C.POINTER(R)]
    f.restype = C.c_bool
lib.rational_cmp.argtypes = [R, R, C.POINTER(C.c_int)]
lib.rational_cmp.restype = C.c_bool
rng = random.Random(42)
count = 0
for _ in range(10000):
    a = Fraction(rng.randint(-100000, 100000), rng.randint(1, 100000))
    b = Fraction(rng.randint(-100000, 100000), rng.randint(1, 100000))
    ar, br = R(a.numerator, a.denominator), R(b.numerator, b.denominator)
    for name, expected in [('add', a+b), ('sub', a-b), ('mul', a*b)]:
        out = R()
        assert getattr(lib, 'rational_'+name)(ar, br, C.byref(out))
        assert (out.num, out.den) == (expected.numerator, expected.denominator)
        count += 1
    result = C.c_int()
    assert lib.rational_cmp(ar, br, C.byref(result))
    assert result.value == (a > b) - (a < b)
    count += 1
print(f'PASS: {count} rational operations versus fractions.Fraction')
