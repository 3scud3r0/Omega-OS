"""Ensure V2 refuses V1 media without silently overwriting old checkpoints."""
import hashlib
from pathlib import Path
import struct
import subprocess
import sys
path=Path('build/v1-preserved.img')
with path.open('wb') as f:
    f.truncate(8*1024*1024)
    f.seek(4096)
    f.write(struct.pack('<Q',0x314c53534147454d))
before=hashlib.sha256(path.read_bytes()).digest()
p=subprocess.Popen([sys.argv[1],'-kernel','omega_os.elf','-drive',f'file={path},if=virtio,format=raw',
                    '-smp','2','-m','128M','-serial','stdio','-display','none','-no-reboot'],
                   stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
try:
    out,_=p.communicate(timeout=2)
except subprocess.TimeoutExpired:
    p.kill()
    out,_=p.communicate(timeout=5)
assert b'V1 disk needs offline migration; refusing overwrite' in out,out.decode(errors='replace')
assert hashlib.sha256(path.read_bytes()).digest()==before
Path('build/v1-refusal.log').write_bytes(out)
print('PASS V1 format refused; disk image unchanged byte for byte')
