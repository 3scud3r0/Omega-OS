"""Corrupt a payload exclusive to the latest root; recover the complete backup."""
from hardware import *
path=Path('build/v3-corrupt.img');sparse_copy(Path('build/v3-mutation-0.img'),path)
old,new=roots(path)[-2:]
assert 5 in new[1] and (5 not in old[1] or old[1][5][0]!=new[1][5][0])
with path.open('r+b') as f:
    f.seek(new[1][5][0]*512);f.write(b'BROKEN-COGNITIVE-PAGE');f.flush();os.fsync(f.fileno())
assert roots(path)[-1][0]==old[0]
boot(path,'v3-corrupt-reboot')
page=roots(path)[-1][1][5][1]
assert struct.unpack_from('<2q',page,16)==(24,1)
assert struct.unpack_from('<2q',page,72)==(24,1)
print('PASS complete-root fallback after corruption of the newest cognitive payload')
