"""V3 storage, in-guest compiler and original Omega engines acceptance."""
from hardware import *
b=Path('omega_os.elf').read_bytes(); offset=b.find(struct.pack('<I',0x1badb002),0,8192)
assert offset>=0 and offset%4==0 and sum(struct.unpack_from('<3I',b,offset))&0xffffffff==0
image=Path('build/v3-test.img'); create(image)
boot(image,'v3-first'); assert epochs(roots(image)[-1][1])==(65536,65536)
boot(image,'v3-reboot'); assert epochs(roots(image)[-1][1])==(131072,131072)
pages=roots(image)[-1][1]
assert pages[262143][1][:16]==b'OMEGA-GIB-SPARSE'
cog=pages[5][1];assert cog[:12]==b'OMEGA-COG-V3'
assert struct.unpack_from('<2q',cog,256)==(1,2)
assert struct.unpack_from('<2q',cog,16)==(24,1) and struct.unpack_from('<2q',cog,72)==(24,1)
assert struct.unpack_from('<3Q',cog,40)==(1,2,1)
for stage in range(1,9):
    cut=Path(f'build/v3-cut-{stage}.img');sparse_copy(image,cut);before=roots(cut)[-1]
    boot(cut,f'v3-cut-{stage}',stage);after=roots(cut)[-1]
    assert epochs(after[1]) in [(131072,131072),(131073,131073)],'mixed transaction epochs'
    if stage<3 or stage>=5:assert epochs(after[1])==(131072,131072)
    if stage==4:assert after[0]==before[0]+1 and epochs(after[1])==(131073,131073)
    if stage==8:assert after[0]==before[0]+3
    boot(cut,f'v3-cut-{stage}-reboot')
    got=epochs(roots(cut)[-1][1]);assert got[0]==got[1]
small=Path('build/v3-small.img');create(small,1024*1024)
text=finish([launch(small,'v3-gc-stress',stress=True)])[0]
assert text.count('SLS GC compacted')>=4
assert epochs(roots(small)[-1][1])==(65696,65696)
assert small.stat().st_size==1024*1024
boot(small,'v3-gc-reboot');assert epochs(roots(small)[-1][1])==(131232,131232)
for index,(source,expected) in enumerate([
    ('tensor result = (1/3+1/6)*12; mostrar result;', (6,1)),
    ('tensor result = -(5/2)+1/4; mostrar result;', (-9,4)),
    ('tensor x=2; tensor result=x*x+1; mostrar result;', (5,1))]):
    case=Path(f'build/v3-mutation-{index}.img');create(case)
    finish([launch(case,f'v3-mutation-{index}',source=source)])
    page=roots(case)[-1][1][5][1]
    assert struct.unpack_from('<2q',page,16)==expected and struct.unpack_from('<2q',page,72)==expected
prior=roots(image)[-1][1][5][1]
job=launch(image,'v3-rejected-mutation',source='tensor result=1/0; mostrar result;')
p=job[0]
try:
    out,_=p.communicate(timeout=3)
except subprocess.TimeoutExpired:
    p.kill();out,_=p.communicate(timeout=5)
assert b'REJECTED V3: mutation quarantined' in out and b'FAIL:' not in out
assert b'fresh Ring3 PID 7' not in out
assert roots(image)[-1][1][5][1]==prior
Path('build/v3-rejected-mutation.log').write_bytes(out)
print('PASS invalid mutation quarantined with original cognitive checkpoint intact')
print('PASS V3 GC repeated arena reuse in fixed 1-MiB disk; 8 SIGKILL boundaries; original Omega Ring3 compiler/egraph/PUCT and dynamic child')
